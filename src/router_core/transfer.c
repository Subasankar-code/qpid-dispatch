/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "router_core_private.h"
#include "exchange_bindings.h"
#include "delivery.h"
#include <qpid/dispatch/amqp.h>
#include <stdio.h>
#include <inttypes.h>


//==================================================================================
// Internal Functions
//==================================================================================

static void qdr_link_deliver_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_link_flow_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_send_to_CT(qdr_core_t *core, qdr_action_t *action, bool discard);


//==================================================================================
// Interface Functions
//==================================================================================

qdr_delivery_t *qdr_link_deliver(qdr_link_t *link, qd_message_t *msg, qd_iterator_t *ingress,
                                 bool settled, qd_bitmask_t *link_exclusion, int ingress_index)
{
    qdr_action_t   *action = qdr_action(qdr_link_deliver_CT, "link_deliver");
    qdr_delivery_t *dlv    = new_qdr_delivery_t();

    ZERO(dlv);
    set_safe_ptr_qdr_link_t(link, &dlv->link_sp);
    dlv->msg            = msg;
    dlv->to_addr        = 0;
    dlv->origin         = ingress;
    dlv->settled        = settled;
    dlv->presettled     = settled;
    dlv->link_exclusion = link_exclusion;
    dlv->ingress_index  = ingress_index;
    dlv->error          = 0;
    dlv->disposition    = 0;

    qdr_delivery_incref(dlv, "qdr_link_deliver - newly created delivery, add to action list");
    qdr_delivery_incref(dlv, "qdr_link_deliver - protect returned value");

    action->args.connection.delivery = dlv;
    action->args.connection.more = !qd_message_receive_complete(msg);
    qdr_action_enqueue(link->core, action);
    return dlv;
}


qdr_delivery_t *qdr_link_deliver_to(qdr_link_t *link, qd_message_t *msg,
                                    qd_iterator_t *ingress, qd_iterator_t *addr,
                                    bool settled, qd_bitmask_t *link_exclusion, int ingress_index)
{
    qdr_action_t   *action = qdr_action(qdr_link_deliver_CT, "link_deliver");
    qdr_delivery_t *dlv    = new_qdr_delivery_t();

    ZERO(dlv);
    set_safe_ptr_qdr_link_t(link, &dlv->link_sp);
    dlv->msg            = msg;
    dlv->to_addr        = addr;
    dlv->origin         = ingress;
    dlv->settled        = settled;
    dlv->presettled     = settled;
    dlv->link_exclusion = link_exclusion;
    dlv->ingress_index  = ingress_index;
    dlv->error          = 0;
    dlv->disposition    = 0;

    qdr_delivery_incref(dlv, "qdr_link_deliver_to - newly created delivery, add to action list");
    qdr_delivery_incref(dlv, "qdr_link_deliver_to - protect returned value");

    action->args.connection.delivery = dlv;
    action->args.connection.more = !qd_message_receive_complete(msg);
    qdr_action_enqueue(link->core, action);
    return dlv;
}


qdr_delivery_t *qdr_link_deliver_to_routed_link(qdr_link_t *link, qd_message_t *msg, bool settled,
                                                const uint8_t *tag, int tag_length,
                                                uint64_t disposition, pn_data_t* disposition_data)
{
    qdr_action_t   *action = qdr_action(qdr_link_deliver_CT, "link_deliver");
    qdr_delivery_t *dlv    = new_qdr_delivery_t();

    ZERO(dlv);
    set_safe_ptr_qdr_link_t(link, &dlv->link_sp);
    dlv->msg          = msg;
    dlv->settled      = settled;
    dlv->presettled   = settled;
    dlv->error        = 0;
    dlv->disposition  = 0;

    qdr_delivery_read_extension_state(dlv, disposition, disposition_data, true);
    qdr_delivery_incref(dlv, "qdr_link_deliver_to_routed_link - newly created delivery, add to action list");
    qdr_delivery_incref(dlv, "qdr_link_deliver_to_routed_link - protect returned value");

    action->args.connection.delivery = dlv;
    action->args.connection.more = !qd_message_receive_complete(msg);
    action->args.connection.tag_length = tag_length;
    assert(tag_length <= QDR_DELIVERY_TAG_MAX);
    memcpy(action->args.connection.tag, tag, tag_length);
    qdr_action_enqueue(link->core, action);
    return dlv;
}


int qdr_link_process_deliveries(qdr_core_t *core, qdr_link_t *link, int credit)
{
    qdr_connection_t *conn = link->conn;
    qdr_delivery_t   *dlv;
    int               offer   = -1;
    bool              settled = false;
    bool              send_complete = false;
    int               num_deliveries_completed = 0;

    if (link->link_direction == QD_OUTGOING) {

        // If a detach has been received on the link, there is no need to process deliveries on the link.
        if (link->detach_received)
            return 0;

        while (credit > 0) {
            sys_mutex_lock(conn->work_lock);
            dlv = DEQ_HEAD(link->undelivered);
            if (dlv) {
                qdr_delivery_incref(dlv, "qdr_link_process_deliveries - holding the undelivered delivery locally");
                uint64_t new_disp = 0;

                // DISPATCH-1302 race hack fix: There is a race between the CORE thread
                // and the outbound (this) thread over settlement. It occurs when the CORE
                // thread is trying to propagate settlement to a peer (this delivery)
                // while this thread is in core->deliver_handler.  This can result in the
                // CORE thread NOT pushing the peer delivery change since it is not yet off of
                // the undelivered list, while this thread does not settle because it missed
                // the settled flag update.
                do {
                    settled = dlv->settled;
                    sys_mutex_unlock(conn->work_lock);
                    new_disp = core->deliver_handler(core->user_context, link, dlv, settled);
                    sys_mutex_lock(conn->work_lock);
                } while (settled != dlv->settled);  // oops missed the settlement
                send_complete = qdr_delivery_send_complete(dlv);
                if (send_complete) {
                    //
                    // The entire message has been sent. It is now the appropriate time to have the delivery removed
                    // from the head of the undelivered list and move it to the unsettled list if it is not settled.
                    //
                    num_deliveries_completed++;

                    credit--;
                    link->credit_to_core--;
                    link->total_deliveries++;

                    // DISPATCH-1153:
                    // If the undelivered list is cleared the link may have detached.  Stop processing.
                    offer = DEQ_SIZE(link->undelivered);
                    if (offer == 0) {
                        qdr_delivery_decref(core, dlv, "qdr_link_process_deliveries - release local reference - closed link");
                        sys_mutex_unlock(conn->work_lock);
                        return num_deliveries_completed;
                    }

                    assert(dlv == DEQ_HEAD(link->undelivered));
                    DEQ_REMOVE_HEAD(link->undelivered);
                    dlv->link_work = 0;

                    if (settled) {
                        dlv->where = QDR_DELIVERY_NOWHERE;
                        qdr_delivery_decref(core, dlv, "qdr_link_process_deliveries - remove from undelivered list");
                    } else {
                        DEQ_INSERT_TAIL(link->unsettled, dlv);
                        dlv->where = QDR_DELIVERY_IN_UNSETTLED;
                        qd_log(core->log, QD_LOG_DEBUG, "Delivery transfer:  dlv:%lx qdr_link_process_deliveries: undelivered-list -> unsettled-list", (long) dlv);
                    }
                }
                else {
                    qdr_delivery_decref(core, dlv, "qdr_link_process_deliveries - release local reference - not send_complete");

                    //
                    // The message is still being received/sent.
                    // 1. We cannot remove the delivery from the undelivered list.
                    //    This delivery needs to stay at the head of the undelivered list until the entire message
                    //    has been sent out i.e other deliveries in the undelivered list have to wait before this
                    //    entire large delivery is sent out
                    // 2. We need to call deliver_handler so any newly arrived bytes can be pushed out
                    // 3. We need to break out of this loop otherwise a thread will keep spinning in here until
                    //    the entire message has been sent out.
                    //
                    sys_mutex_unlock(conn->work_lock);

                    //
                    // Note here that we are not incrementing num_deliveries_processed. Since this delivery is
                    // still coming in or still being sent out, we cannot consider this delivery as fully processed.
                    //
                    return num_deliveries_completed;
                }
                sys_mutex_unlock(conn->work_lock);

                // the core will need to update the delivery's disposition
                if (new_disp)
                    qdr_delivery_update_disposition(core, dlv, new_disp, true, 0, 0, false);
                qdr_delivery_decref(core, dlv, "qdr_link_process_deliveries - release local reference - done processing");
            } else {
                sys_mutex_unlock(conn->work_lock);
                break;
            }
        }

        if (offer != -1)
            core->offer_handler(core->user_context, link, offer);
    }

    return num_deliveries_completed;
}


void qdr_link_flow(qdr_core_t *core, qdr_link_t *link, int credit, bool drain_mode)
{
    qdr_action_t *action = qdr_action(qdr_link_flow_CT, "link_flow");

    //
    // Compute the number of credits now available that we haven't yet given
    // incrementally to the router core.  i.e. convert absolute credit to
    // incremental credit.
    //
    if (link->drain_mode && !drain_mode) {
        link->credit_to_core = 0;   // credit calc reset when coming out of drain mode
    } else {
        credit -= link->credit_to_core;
        if (credit < 0)
            credit = 0;
        link->credit_to_core += credit;
    }

    action->args.connection.link   = link;
    action->args.connection.credit = credit;
    action->args.connection.drain  = drain_mode;

    qdr_action_enqueue(core, action);
}


void qdr_send_to1(qdr_core_t *core, qd_message_t *msg, qd_iterator_t *addr, bool exclude_inprocess, bool control)
{
    qdr_action_t *action = qdr_action(qdr_send_to_CT, "send_to");
    action->args.io.address           = qdr_field_from_iter(addr);
    action->args.io.message           = qd_message_copy(msg);
    action->args.io.exclude_inprocess = exclude_inprocess;
    action->args.io.control           = control;

    qdr_action_enqueue(core, action);
}


void qdr_send_to2(qdr_core_t *core, qd_message_t *msg, const char *addr, bool exclude_inprocess, bool control)
{
    qdr_action_t *action = qdr_action(qdr_send_to_CT, "send_to");
    action->args.io.address           = qdr_field(addr);
    action->args.io.message           = qd_message_copy(msg);
    action->args.io.exclude_inprocess = exclude_inprocess;
    action->args.io.control           = control;

    qdr_action_enqueue(core, action);
}


//==================================================================================
// In-Thread Functions
//==================================================================================


static void qdr_link_flow_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_link_t *link      = action->args.connection.link;
    int  credit           = action->args.connection.credit;
    bool drain            = action->args.connection.drain;
    bool activate         = false;
    bool drain_was_set    = !link->drain_mode && drain;
    qdr_link_work_t *work = 0;
    
    link->drain_mode = drain;

    //
    // If the link was stalled due to internal backpressure from the transport, put it
    // on the links-with-work list and activate the connection to resume sending.
    //
    if (link->stalled_outbound) {
        link->stalled_outbound = false;
        if (DEQ_SIZE(link->undelivered) > 0) {
            // Adding this work at priority 0.
            qdr_add_link_ref(link->conn->links_with_work, link, QDR_LINK_LIST_CLASS_WORK);
            activate = true;
        }
    }

    if (link->core_endpoint) {
        qdrc_endpoint_do_flow_CT(core, link->core_endpoint, credit, drain);
    } else if (link->connected_link) {
        //
        // If this is an attach-routed link, propagate the flow data downrange.
        // Note that the credit value is incremental.
        //
        qdr_link_t *clink = link->connected_link;

        if (clink->link_direction == QD_INCOMING)
            qdr_link_issue_credit_CT(core, link->connected_link, credit, drain);
        else {
            work = new_qdr_link_work_t();
            ZERO(work);
            work->work_type = QDR_LINK_WORK_FLOW;
            work->value     = credit;
            if (drain)
                work->drain_action = QDR_LINK_WORK_DRAIN_ACTION_DRAINED;
            qdr_link_enqueue_work_CT(core, clink, work);
        }
    } else {
        if (link->attach_count == 1)
            //
            // The link is half-open.  Store the pending credit to be dealt with once the link is
            // progressed to the next step.
            //
            link->credit_stored += credit;

        //
        // Handle the replenishing of credit outbound
        //
        if (link->link_direction == QD_OUTGOING && (credit > 0 || drain_was_set)) {
            if (drain_was_set) {
                work = new_qdr_link_work_t();
                ZERO(work);
                work->work_type    = QDR_LINK_WORK_FLOW;
                work->drain_action = QDR_LINK_WORK_DRAIN_ACTION_DRAINED;
            }

            sys_mutex_lock(link->conn->work_lock);
            if (work)
                DEQ_INSERT_TAIL(link->work_list, work);
            if (DEQ_SIZE(link->undelivered) > 0 || drain_was_set) {
                // Adding this work at priority 0.
                qdr_add_link_ref(link->conn->links_with_work, link, QDR_LINK_LIST_CLASS_WORK);
                activate = true;
            }
            sys_mutex_unlock(link->conn->work_lock);
        } else if (link->link_direction == QD_INCOMING) {
            if (drain) {
                link->credit_pending = link->capacity;
            }
        }
    }

    //
    // Activate the connection if we have deliveries to send or drain mode was set.
    //
    if (activate)
        qdr_connection_activate_CT(core, link->conn);
}


/**
 * Return the number of outbound paths to destinations that this address has.
 * Note that even if there are more than zero paths, the destination still may
 * be unreachable (e.g. an rnode next hop with no link).
 */
static long qdr_addr_path_count_CT(qdr_address_t *addr)
{
    if (!addr)
        return 0;

    long rc = ((long) DEQ_SIZE(addr->subscriptions)
               + (long) DEQ_SIZE(addr->rlinks)
               + (long) qd_bitmask_cardinality(addr->rnodes));
    if (addr->exchange)
        rc += qdr_exchange_binding_count(addr->exchange)
            + ((qdr_exchange_alternate_addr(addr->exchange)) ? 1 : 0);
    return rc;
}


static void qdr_link_forward_CT(qdr_core_t *core, qdr_link_t *link, qdr_delivery_t *dlv, qdr_address_t *addr, bool more)
{
    qdr_link_t *dlv_link = qdr_delivery_link(dlv);

    assert(dlv_link == link);

    if (!dlv_link)
        return;

    if (dlv_link->link_type == QD_LINK_ENDPOINT && !dlv_link->fallback)
        core->deliveries_ingress++;

    if (addr
        && addr == link->owning_addr
        && qdr_addr_path_count_CT(addr) == 0
        && (link->fallback || qdr_addr_path_count_CT(addr->fallback) == 0)) {
        //
        // We are trying to forward a delivery on an address that has no outbound paths
        // AND the incoming link is targeted (not anonymous).
        //
        // We shall release the delivery (it is currently undeliverable).
        //
        if (dlv->settled) {
            // Increment the presettled_dropped_deliveries on the in_link
            link->dropped_presettled_deliveries++;
            if (dlv_link->link_type == QD_LINK_ENDPOINT)
                core->dropped_presettled_deliveries++;

            //
            // The delivery is pre-settled. Call the qdr_delivery_release_CT so if this delivery is multi-frame
            // we can restart receiving the delivery in case it is stalled. Note that messages will not
            // *actually* be released because these are presettled messages.
            //
            qdr_delivery_release_CT(core, dlv);
        } else {
            qdr_delivery_release_CT(core, dlv);

            //
            // Drain credit on the link if it is not in an edge connection
            //
            if (!link->edge)
                qdr_link_issue_credit_CT(core, link, 0, true);
        }

        //
        // If the distribution is multicast or it's on an edge connection, we will replenish the credit.
        // Otherwise, we will allow the credit to drain.
        //
        if (link->edge || qdr_is_addr_treatment_multicast(link->owning_addr))
            qdr_link_issue_credit_CT(core, link, 1, false);
        else
            link->credit_pending++;

        qdr_delivery_decref_CT(core, dlv, "qdr_link_forward_CT - removed from action (no path)");
        return;
    }

    int fanout = 0;

    dlv->multicast = qdr_is_addr_treatment_multicast(addr);

    if (addr) {
        fanout = qdr_forward_message_CT(core, addr, dlv->msg, dlv, false, link->link_type == QD_LINK_CONTROL);
        if (link->link_type != QD_LINK_CONTROL && link->link_type != QD_LINK_ROUTER) {
            if (!link->fallback)
                addr->deliveries_ingress++;

            if (qdr_connection_route_container(link->conn)) {
                addr->deliveries_ingress_route_container++;
                core->deliveries_ingress_route_container++;
            }

        }
        link->total_deliveries++;
    }

    //
    // There is no address that we can send this delivery to, which means the addr was not found in our hastable. This
    // can be because there were no receivers or because the address was not defined in the config file.
    // If the treatment for such addresses is set to be unavailable, we send back a rejected disposition and detach the link
    //
    else if (core->qd->default_treatment == QD_TREATMENT_UNAVAILABLE) {
        dlv->disposition = PN_REJECTED;
        dlv->error = qdr_error(QD_AMQP_COND_NOT_FOUND, "Deliveries cannot be sent to an unavailable address");
        qdr_delivery_push_CT(core, dlv);

        //
        // We will not detach this link because this could be anonymous sender. We don't know
        // which address the sender will be sending to next
        // If this was not an anonymous sender, the initial attach would have been rejected if the target address was unavailable.
        //
        return;
    }

    //
    // If the anonymous delivery could not be sent anywhere (fanout = 0) and it is not multicasted, try sending it over
    // the anonymous link.
    //
    if (fanout == 0 && !dlv->multicast && link->owning_addr == 0 && dlv->to_addr != 0) {
        if (core->edge_conn_addr && link->conn->role != QDR_ROLE_EDGE_CONNECTION) {
            qdr_address_t *sender_address = core->edge_conn_addr(core->edge_context);
            if (sender_address && sender_address != addr)
                fanout += qdr_forward_message_CT(core, sender_address, dlv->msg, dlv, false, link->link_type == QD_LINK_CONTROL);
        }
    }

    //
    // If the fanout is still zero, check to see if there is a fallback address and
    // route via the fallback if present.  Don't do fallback forwarding if this link is
    // itself associated with a fallback destination.
    //
    if (fanout == 0 && !!addr && !!addr->fallback && !link->fallback) {
        const char          *key      = (const char*) qd_hash_key_by_handle(addr->fallback->hash_handle);
        qd_composed_field_t *to_field = qd_compose_subfield(0);
        qd_compose_insert_string(to_field, key + 2);
        qd_message_set_to_override_annotation(dlv->msg, to_field);
        qd_message_set_phase_annotation(dlv->msg, key[1] - '0');
        fanout = qdr_forward_message_CT(core, addr->fallback, dlv->msg, dlv, false, link->link_type == QD_LINK_CONTROL);
        if (fanout > 0) {
            addr->deliveries_redirected++;
            core->deliveries_redirected++;
        }
    }

    if (fanout == 0) {
        //
        // Message was not delivered, drop the delivery.
        //
        // If the delivery is not settled, release it.
        //
        if (!dlv->settled)
            qdr_delivery_release_CT(core, dlv);

        //
        // Decrementing the delivery ref count for the action
        //
        qdr_delivery_decref_CT(core, dlv, "qdr_link_forward_CT - removed from action (1)");
        qdr_link_issue_credit_CT(core, link, 1, false);
    } else if (fanout > 0) {
        if (dlv->settled || dlv->multicast) {
            //
            // The delivery is settled.  Keep it off the unsettled list and issue
            // replacement credit for it now.
            //
            qdr_link_issue_credit_CT(core, link, 1, false);
            if (!more) {
                //
                // This decref is for the action ref
                //
                qdr_delivery_decref_CT(core, dlv, "qdr_link_forward_CT - removed from action (2)");
            }
            else {
                //
                // The message is still coming through since receive_complete is false. We have to put this delivery in the settled list.
                // We need to do this because we have linked this delivery to a peer.
                // If this connection goes down, we will have to unlink peer so that peer knows that its peer is not-existent anymore
                // and need to tell the other side that the message has been aborted.
                //

                //
                // Again, don't bother decrementing then incrementing the ref_count, we are still using the action ref count
                //
                DEQ_INSERT_TAIL(link->settled, dlv);
                dlv->where = QDR_DELIVERY_IN_SETTLED;
                qd_log(core->log, QD_LOG_DEBUG, "Delivery transfer:  dlv:%lx qdr_link_forward_CT: action-list -> settled-list", (long) dlv);
            }
        } else {
            //
            // Again, don't bother decrementing then incrementing the ref_count
            //
            DEQ_INSERT_TAIL(link->unsettled, dlv);
            dlv->where = QDR_DELIVERY_IN_UNSETTLED;
            qd_log(core->log, QD_LOG_DEBUG, "Delivery transfer:  dlv:%lx qdr_link_forward_CT: action-list -> unsettled-list", (long) dlv);

            //
            // If the delivery was received on an inter-router link, issue the credit
            // now.  We don't want to tie inter-router link flow control to unsettled
            // deliveries because it increases the risk of credit starvation if there
            // are many addresses sharing the link.
            //
            if (link->link_type == QD_LINK_ROUTER || link->edge)
                qdr_link_issue_credit_CT(core, link, 1, false);
        }
    }
}


static void qdr_link_deliver_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_delivery_t *dlv  = action->args.connection.delivery;
    bool            more = action->args.connection.more;
    qdr_link_t     *link = qdr_delivery_link(dlv);

    if (!link)
        return;

    //
    // Record the ingress time so we can track the age of this delivery.
    //
    dlv->ingress_time = core->uptime_ticks;

    //
    // If the link is an edge link, mark this delivery as via-edge
    //
    dlv->via_edge = link->edge;

    //
    // If this link has a core_endpoint, direct deliveries to that endpoint.
    //
    if (!!link->core_endpoint) {
        qdrc_endpoint_do_deliver_CT(core, link->core_endpoint, dlv);
        return;
    }

    if (link->connected_link) {
        if (link->link_direction == QD_INCOMING)
            core->deliveries_ingress++;

        //
        // If this is an attach-routed link, put the delivery directly onto the peer link
        //
        qdr_delivery_t *peer = qdr_forward_new_delivery_CT(core, dlv, link->connected_link, dlv->msg);

        qdr_delivery_copy_extension_state(dlv, peer, true);

        //
        // Copy the delivery tag.  For link-routing, the delivery tag must be preserved.
        //
        peer->tag_length = action->args.connection.tag_length;
        memcpy(peer->tag, action->args.connection.tag, peer->tag_length);

        qdr_forward_deliver_CT(core, link->connected_link, peer);

        link->total_deliveries++;

        if (!dlv->settled) {
            DEQ_INSERT_TAIL(link->unsettled, dlv);
            dlv->where = QDR_DELIVERY_IN_UNSETTLED;
            qd_log(core->log, QD_LOG_DEBUG, "Delivery transfer:  dlv:%lx qdr_link_deliver_CT: action-list -> unsettled-list", (long) dlv);
        } else {
            //
            // If the delivery is settled, decrement the ref_count on the delivery.
            // This count was the owned-by-action count.
            //
            qdr_delivery_decref_CT(core, dlv, "qdr_link_deliver_CT - removed from action");
        }
        return;
    }

    //
    // NOTE: The link->undelivered list does not need to be protected by the
    //       connection's work lock for incoming links.  This protection is only
    //       needed for outgoing links.
    //

    if (DEQ_IS_EMPTY(link->undelivered)) {
        qdr_link_ref_t *temp_rlink = 0;
        qdr_address_t *addr = link->owning_addr;
        if (!addr && dlv->to_addr) {
            qdr_connection_t *conn = link->conn;
            if (conn && conn->tenant_space)
                qd_iterator_annotate_space(dlv->to_addr, conn->tenant_space, conn->tenant_space_len);
            qd_hash_retrieve(core->addr_hash, dlv->to_addr, (void**) &addr);

            if (!addr) {
                //
                // This is an anonymous delivery but the address that it wants sent to is
                // not in this router's address table. We will send this delivery up the
                // anonymous link to the interior router (if this is an edge router).
                // Only edge routers have a non null core->edge_conn_addr
                //
                if (core->edge_conn_addr && link->conn->role != QDR_ROLE_EDGE_CONNECTION) {
                    qdr_address_t *sender_address = core->edge_conn_addr(core->edge_context);
                    if (sender_address) {
                        addr = sender_address;
                    }
                }
            }
            else {
                //
                // (core->edge_conn_addr is non-zero ONLY on edge routers. So there is no need to check if the
                // core->router_mode is edge.
                //
                // The connection on which the delivery arrived should not be QDR_ROLE_EDGE_CONNECTION because
                // we do not want to send it back over the same connections
                //
                if (core->edge_conn_addr && link->conn->role != QDR_ROLE_EDGE_CONNECTION && qdr_is_addr_treatment_multicast(addr)) {
                    qdr_address_t *sender_address = core->edge_conn_addr(core->edge_context);
                    if (sender_address && sender_address != addr) {
                        qdr_link_ref_t *sender_rlink = DEQ_HEAD(sender_address->rlinks);
                        if (sender_rlink) {
                            temp_rlink = new_qdr_link_ref_t();
                            DEQ_ITEM_INIT(temp_rlink);
                            temp_rlink->link = sender_rlink->link;
                            DEQ_INSERT_TAIL(addr->rlinks, temp_rlink);
                        }
                    }
                }
            }
        }

        //
        // Deal with any delivery restrictions for this address.
        //
        if (addr && addr->router_control_only && link->link_type != QD_LINK_CONTROL) {
            qdr_delivery_release_CT(core, dlv);
            qdr_link_issue_credit_CT(core, link, 1, false);
            qdr_delivery_decref_CT(core, dlv, "qdr_link_deliver_CT - removed from action on restricted access");
        } else {
            //
            // Give the action reference to the qdr_link_forward function. Don't decref/incref.
            //
            qdr_link_forward_CT(core, link, dlv, addr, more);
        }

        if (addr && temp_rlink) {
            DEQ_REMOVE(addr->rlinks, temp_rlink);
            free_qdr_link_ref_t(temp_rlink);
        }
    } else {
        assert(false);
        //
        // Take the action reference and use it for undelivered.  Don't decref/incref.
        //
        DEQ_INSERT_TAIL(link->undelivered, dlv);
        dlv->where = QDR_DELIVERY_IN_UNDELIVERED;
        qd_log(core->log, QD_LOG_DEBUG, "Delivery transfer:  dlv:%lx qdr_link_deliver_CT: action-list -> undelivered-list", (long) dlv);
    }
}


static void qdr_send_to_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (!discard) {
        qdr_in_process_send_to_CT(core,
                                  qdr_field_iterator(action->args.io.address),
                                  action->args.io.message,
                                  action->args.io.exclude_inprocess,
                                  action->args.io.control);
    }

    qdr_field_free(action->args.io.address);
    qd_message_free(action->args.io.message);
}


/**
 * forward an in-process message based on the destination address
 */
void qdr_in_process_send_to_CT(qdr_core_t *core, qd_iterator_t *address, qd_message_t *msg, bool exclude_inprocess, bool control)
{
    qdr_address_t *addr = 0;

    qd_iterator_reset_view(address, ITER_VIEW_ADDRESS_HASH);
    qd_hash_retrieve(core->addr_hash, address, (void**) &addr);
    if (addr) {
        //
        // Forward the message.  We don't care what the fanout count is.
        //
        (void) qdr_forward_message_CT(core, addr, msg, 0, exclude_inprocess, control);
        addr->deliveries_from_container++;
    } else
        qd_log(core->log, QD_LOG_DEBUG, "In-process send to an unknown address");
}


/**
 * Add link-work to provide credit to the link in an IO thread
 */
void qdr_link_issue_credit_CT(qdr_core_t *core, qdr_link_t *link, int credit, bool drain)
{
    assert(link->link_direction == QD_INCOMING);

    bool drain_changed = link->drain_mode ^ drain;
    link->drain_mode   = drain;

    if (link->credit_pending > 0)
        link->credit_pending = link->credit_pending > credit ? link->credit_pending - credit : 0;

    if (!drain_changed && credit == 0)
        return;

    qdr_link_work_t *work = new_qdr_link_work_t();
    ZERO(work);

    work->work_type = QDR_LINK_WORK_FLOW;
    work->value     = credit;

    if (drain_changed)
        work->drain_action = drain ? QDR_LINK_WORK_DRAIN_ACTION_SET : QDR_LINK_WORK_DRAIN_ACTION_CLEAR;

    qdr_link_enqueue_work_CT(core, link, work);
}


/**
 * Attempt to push all of the undelivered deliveries on an incoming link downrange.
 */
void qdr_drain_inbound_undelivered_CT(qdr_core_t *core, qdr_link_t *link, qdr_address_t *addr)
{
    if (DEQ_SIZE(link->undelivered) > 0) {
        //
        // Move all the undelivered to a local list in case not all can be delivered.
        // We don't want to loop here forever putting the same messages on the undelivered
        // list.
        //
        qdr_delivery_list_t deliveries;
        DEQ_MOVE(link->undelivered, deliveries);

        qdr_delivery_t *dlv = DEQ_HEAD(deliveries);
        while (dlv) {
            DEQ_REMOVE_HEAD(deliveries);
            qdr_link_forward_CT(core, link, dlv, addr, false);
            dlv = DEQ_HEAD(deliveries);
        }
    }
}


/**
 * This function should be called after adding a new destination (subscription, local link,
 * or remote node) to an address.  If this address now has exactly one destination (i.e. it
 * transitioned from unreachable to reachable), make sure any unstarted in-links are issued
 * initial credit.
 *
 * Also, check the inlinks to see if there are undelivered messages.  If so, drain them to
 * the forwarder.
 */
void qdr_addr_start_inlinks_CT(qdr_core_t *core, qdr_address_t *addr)
{
    if (qdr_addr_path_count_CT(addr) == 1 || (!!addr->fallback && qdr_addr_path_count_CT(addr->fallback) == 1)) {
        qdr_link_ref_t *ref = DEQ_HEAD(addr->inlinks);
        while (ref) {
            qdr_link_t *link = ref->link;

            //
            // Issue credit to stalled links
            //
            if (link->credit_pending > 0)
                qdr_link_issue_credit_CT(core, link, link->credit_pending, false);

            //
            // Drain undelivered deliveries via the forwarder
            //
            qdr_drain_inbound_undelivered_CT(core, link, addr);

            ref = DEQ_NEXT(ref);
        }

        if (!!addr->fallback_for)
            qdr_addr_start_inlinks_CT(core, addr->fallback_for);
    }
}
