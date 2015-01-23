#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License
#

"""
Python agent for dispatch router.

Implements the server side of the AMQP management protocol for the dispatch router.
Manages a set of manageable Entities that can be Created, Read, Updated and Deleted.
Entity types are described by the schema in qdrouter.json.

HOW IT WORKS:

There are 3 types of entity:
1 .Entities created via this agent, e.g. configuration entities.
   Attributes are pushed into C code and cached by the agent.
2. Entities created in C code and registered with the agent.
   Attributes are pulled from C code before handling an operation.
3. Combined: created as 1. and later registered via 2.
   Attributes pulled from C overide the initial attributes.

THREAD SAFETY:

The agent is always driven by requests arriving in connection threads.
Handling requests is serialized.

Adding/removing/updating entities from C:
- C code registers add/remove of C entities in a thread safe C cache.
- Before handling an operation, the agent:
  1. locks the router
  2. flushes the add/remove cache and adds/removes entities in python cache.
  3. updates *all* known entites with a C implementation.
  4. unlocks the router.
"""

import traceback, sys
from itertools import ifilter, chain
from traceback import format_exc
from threading import Lock
from ctypes import c_void_p, py_object, c_long
from dispatch import IoAdapter, LogAdapter, LOG_INFO, LOG_DEBUG, LOG_ERROR
from qpid_dispatch.management.error import ManagementError, OK, CREATED, NO_CONTENT, STATUS_TEXT, \
    BadRequestStatus, InternalServerErrorStatus, NotImplementedStatus, NotFoundStatus
from qpid_dispatch.management.entity import camelcase
from .. import dispatch_c
from .schema import ValidationError, SchemaEntity, EntityType
from .qdrouter import QdSchema
from ..router.message import Message


def dictstr(d):
    """Stringify a dict in the form 'k=v, k=v ...' instead of '{k:v, ...}'"""
    return ", ".join("%s=%r" % (k, v) for k, v in d.iteritems())

def required_property(prop, request):
    """Raise exception if required property is missing"""
    if not request.properties or prop not in request.properties:
        raise BadRequestStatus("No '%s' property: %s"%(prop, request))
    return request.properties[prop]

def not_implemented(operation, entity_type):
    """Raise NOT_IMPLEMENTED exception"""
    raise NotImplementedStatus("Operation '%s' not implemented on %s"  % (operation, entity_type))

class AtomicCount(object):
    """Simple atomic counter"""
    def __init__(self, count=0):
        self.count = count
        self.lock = Lock()

    def next(self):
        with self.lock:
            n = self.count
            self.count += 1
            return n

class AgentEntity(SchemaEntity):
    """
    Base class for agent entities with operations as well as attributes.
    """

    def __init__(self, agent, entity_type, attributes, validate=True):
        """
        @para agent: Containing L{Agent}
        @param entity_type: L{EntityType}
        @param attributes: Attribute name:value map
        @param validate: If true, validate the entity.
        """
        super(AgentEntity, self).__init__(entity_type, attributes, validate=validate)
        # Direct __dict__ access to avoid validation as schema attributes
        self.__dict__['_agent'] = agent
        self.__dict__['_qd'] = agent.qd
        self.__dict__['_dispatch'] = agent.dispatch

    def validate(self):
        # Set default identity and name if not already set.
        prefix = self.entity_type.short_name + "/"
        if self.attributes.get('identity') is None:
            self.attributes['identity'] = prefix + str(self._identifier())
        elif not self.attributes['identity'].startswith(prefix):
            self.attributes['identity'] = prefix + self.attributes['identity']
        self.attributes.setdefault('name', self.attributes['identity'])
        super(AgentEntity, self).validate()

    def _identifier(self):
        """
        Generate identifier. identity=type/identifier.
        Default is per-type counter, derived classes can override.
        """
        try: counter = type(self)._identifier_count
        except AttributeError: counter = type(self)._identifier_count = AtomicCount()
        return str(counter.next())

    def _refresh(self):
        """Refresh self.attributes from C implementation. No-op if no C impl pointer"""
        return False # Replaced by _set_pointer

    def _set_pointer(self, pointer):
        fname = "qd_entity_refresh_" + self.entity_type.short_name.replace('.', '_')
        refreshfn = self._qd.function(fname, c_long, [py_object, c_void_p])
        def _do_refresh():
            refreshfn(self.attributes, pointer)
            return True
        self.__dict__['_refresh'] = _do_refresh

    def create(self):
        """Subclasses can add extra create actions here"""
        pass

    def read(self, request):
        """Handle read request, default is to return attributes."""
        request_type = self.entity_type.schema.long_name(request.properties.get('type'))
        if request_type and self.type != request_type:
            raise NotFoundStatus("Entity type '%s' does match requested type '%s'" %
                           (self.type, request_type))
        return (OK, self.attributes)

    def update(self, request):
        """Handle update request with new attributes from management client"""
        newattrs = dict(self.attributes, **request.body)
        self.entity_type.validate(newattrs)
        self.attributes = newattrs
        self._update()
        return (OK, self.attributes)

    def _update(self):
        """Subclasses implement update logic here"""
        pass

    def delete(self, request):
        """Handle delete request from client"""
        self._agent.remove(self)
        self._delete()
        return (NO_CONTENT, {})

    def _delete(self):
        """Subclasses implement delete logic here"""
        pass

class ContainerEntity(AgentEntity):

    def create(self):
        self._qd.qd_dispatch_configure_container(self._dispatch, self)

    def _identifier(self):
        self.attributes.setdefault("containerName", "00000000-0000-0000-0000-000000000000")
        return self.attributes["containerName"]

class RouterEntity(AgentEntity):
    def __init__(self, agent, entity_type, attributes=None):
        super(RouterEntity, self).__init__(agent, entity_type, attributes, validate=False)
        # Router is a mix of configuration and operational entity.
        # The "area" attribute is operational not configured.
        # FIXME aconway 2014-12-19: clean this up.
        self._set_pointer(self._dispatch)

    def _identifier(self):
        return self.attributes.get('routerId')

    def create(self):
        self._qd.qd_dispatch_configure_router(self._dispatch, self)


class LogEntity(AgentEntity):
    def __init__(self, agent, entity_type, attributes=None, validate=True):
        # Special defaults for DEFAULT module. 
        if attributes.get("module") == "DEFAULT":
            defaults = dict(enable="info+", timestamp=True, source=False, output="stderr")
            attributes = dict(defaults, **attributes)
        super(LogEntity, self).__init__(agent, entity_type, attributes, validate=True)

    def _identifier(self): return self.attributes.get('module')

    def create(self):
        self._qd.qd_log_entity(self)

    def _update(self):
        self._qd.qd_log_entity(self)

    def _delete(self):
        """Can't actually delete a log source but return it to the default state"""
        self._qd.qd_log_source_reset(self.attributes['module'])

def _addr_port_identifier(entity):
    for attr in ['addr', 'port']: # Set default values if need be
        entity.attributes.setdefault(
            attr, entity.entity_type.attribute(attr).missing_value())
    return "%s:%s" % (entity.attributes['addr'], entity.attributes['port'])

class ListenerEntity(AgentEntity):
    def create(self):
        self._qd.qd_dispatch_configure_listener(self._dispatch, self)
        self._qd.qd_connection_manager_start(self._dispatch)

    def _identifier(self): return _addr_port_identifier(self)

class ConnectorEntity(AgentEntity):
    def create(self):
        self._qd.qd_dispatch_configure_connector(self._dispatch, self)
        self._qd.qd_connection_manager_start(self._dispatch)

    def _identifier(self): return _addr_port_identifier(self)

class FixedAddressEntity(AgentEntity):
    def create(self):
        self._qd.qd_dispatch_configure_address(self._dispatch, self)


class WaypointEntity(AgentEntity):
    def create(self):
        self._qd.qd_dispatch_configure_waypoint(self._dispatch, self)
        self._qd.qd_waypoint_activate_all(self._dispatch)

class ExternalContainerEntity(AgentEntity):
    def create(self):
        self._qd.qd_dispatch_configure_external_container(self._dispatch, self)

class DummyEntity(AgentEntity):

    def callme(self, request):
        return (OK, dict(**request.properties))


class CEntity(AgentEntity):
    """
    Entity that is registered from C code rather than created via management.
    """
    def __init__(self, agent, entity_type, pointer):
        super(CEntity, self).__init__(agent, entity_type, {}, validate=False)
        self._set_pointer(pointer)
        self._refresh()
        self.validate()


class RouterLinkEntity(CEntity): pass

class RouterNodeEntity(CEntity): pass

class RouterAddressEntity(CEntity): pass

class ConnectionEntity(CEntity): pass

class AllocatorEntity(CEntity): pass

class EntityCache(object):
    """
    Searchable cache of entities, can be refreshd from C attributes.
    """
    def __init__(self, agent):
        self.entities = []
        self.pointers = {}
        self.agent = agent
        self.qd = self.agent.qd
        self.schema = agent.schema
        self.log = self.agent.log

    def map_filter(self, function, test):
        """Filter with test then apply function."""
        return map(function, ifilter(test, self.entities))

    def map_type(self, function, type):
        """Apply function to all entities of type, if type is None do all entities"""
        if type is None:
            return map(function, self.entities)
        else:
            if not isinstance(type, EntityType): type = self.schema.entity_type(type)
            return map(function, ifilter(lambda e: e.entity_type.is_a(type), self.entities))

    def add(self, entity, pointer=None):
        """Add an entity. Provide pointer if it is associated with a C entity"""
        self.log(LOG_DEBUG, "Add entity: %s" % entity)
        entity.validate()       # Fill in defaults etc.
        # Validate in the context of the existing entities for uniqueness
        self.schema.validate_full(chain(iter([entity]), iter(self.entities)))
        self.entities.append(entity)
        if pointer: self.pointers[pointer] = entity

    def _remove(self, entity):
        try:
            self.entities.remove(entity)
            self.log(LOG_DEBUG, "Remove %s entity: %s" %
                     (entity.entity_type.short_name, entity.attributes['identity']))
        except ValueError: pass

    def remove(self, entity):
        self._remove(entity)

    def remove_pointer(self, pointer):
        self._remove_pointer(pointer)

    def _remove_pointer(self, pointer):
        if pointer in self.pointers:
            entity = self.pointers[pointer]
            del self.pointers[pointer]
            self._remove(entity)


    def refresh_from_c(self):
        """Refresh entities from the C dispatch runtime"""
        REMOVE, ADD = 0, 1

        def remove_redundant(events):
            """Remove redundant add/remove pairs of events."""
            add = {}            # add[pointer] = index of add event.
            redundant = []      # List of redundant event indexes.
            for i in xrange(len(events)):
                action, type, pointer = events[i]
                if action == ADD:
                    add[pointer] = i
                elif pointer in add: # action == REMOVE and there's an ADD
                    redundant.append(add[pointer])
                    redundant.append(i)
                    del add[pointer]
            for i in sorted(redundant, reverse=True):
                events.pop(i)

        # FIXME aconway 2014-10-23: locking is ugly, push it down into C code.
        self.qd.qd_dispatch_router_lock(self.agent.dispatch)
        try:
            events = []
            self.qd.qd_entity_refresh_begin(events)
            remove_redundant(events)
            for action, type, pointer in events:
                if action == REMOVE:
                    self._remove_pointer(pointer)
                elif action == ADD:
                    entity_type = self.schema.entity_type(type)
                    klass = self.agent.entity_class(entity_type)
                    entity = klass(self.agent, entity_type, pointer)
                    self.add(entity, pointer)
            # Refresh the entity values while the lock is still held.
            for e in self.entities: e._refresh()
        finally:
            self.qd.qd_entity_refresh_end()
            self.qd.qd_dispatch_router_unlock(self.agent.dispatch)

class Agent(object):
    """AMQP managment agent"""

    def __init__(self, dispatch, qd):
        self.qd = qd
        self.dispatch = dispatch
        self.schema = QdSchema()
        self.entities = EntityCache(self)
        self.name = self.identity = 'self'
        self.type = 'org.amqp.management' # AMQP management node type
        self.request_lock = Lock()
        self.log_adapter = LogAdapter("AGENT")

    def log(self, level, text):
        info = traceback.extract_stack(limit=2)[0] # Caller frame info
        self.log_adapter.log(level, text, info[0], info[1])

    def activate(self, address):
        """Register the management address to receive management requests"""
        self.entities.refresh_from_c()
        self.log(LOG_INFO, "Activating management agent on %s" % address)
        self.io = [IoAdapter(self.receive, address),
                   IoAdapter(self.receive, address, True)] # Global

    def entity_class(self, entity_type):
        """Return the class that implements entity_type"""
        class_name = camelcase(entity_type.short_name, capital=True) + 'Entity'
        entity_class = globals().get(class_name)
        if not entity_class:
            raise InternalServerErrorStatus(
                "Can't find implementation '%s' for '%s'" % (class_name, entity_type.name))
        return entity_class

    def create_entity(self, attributes):
        """Create an instance of the implementation class for an entity"""

        if attributes.get('identity') is not None:
            raise BadRequestStatus("'identity' attribute cannot be specified %s" % attributes)
        if attributes.get('type') is None:
            raise BadRequestStatus("No 'type' attribute in %s" % attributes)
        entity_type = self.schema.entity_type(attributes['type'])
        return self.entity_class(entity_type)(self, entity_type, attributes)

    def respond(self, request, status=OK, description=None, body=None):
        """Send a response to the client"""
        if body is None: body = {}
        description = description or STATUS_TEXT[status]
        response = Message(
            address=request.reply_to,
            correlation_id=request.correlation_id,
            properties={'statusCode': status, 'statusDescription': description},
            body=body)
        self.log(LOG_DEBUG, "Agent response:\n  %s\n  Responding to: \n  %s"%(response, request))
        try:
            self.io[0].send(response)
        except:
            self.log(LOG_ERROR, "Can't respond to %s: %s"%(request, format_exc()))

    def receive(self, request, link_id):
        """Called when a management request is received."""
        # Coarse locking, handle one request at a time.
        with self.request_lock:
            self.entities.refresh_from_c()
            self.log(LOG_DEBUG, "Agent request %s on link %s"%(request, link_id))
            def error(e, trace):
                """Raise an error"""
                self.log(LOG_ERROR, "Error dispatching %s: %s\n%s"%(request, e, trace))
                self.respond(request, e.status, e.description)
            try:
                status, body = self.handle(request)
                self.respond(request, status=status, body=body)
            except ManagementError, e:
                error(e, format_exc())
            except ValidationError, e:
                error(BadRequestStatus(str(e)), format_exc())
            except Exception, e:
                error(InternalServerErrorStatus("%s: %s"%(type(e).__name__, e)), format_exc())

    def entity_type(self, type):
        try: return self.schema.entity_type(type)
        except ValidationError, e: raise NotFoundStatus(str(e))

    def handle(self, request):
        """
        Handle a request.
        Dispatch management node requests to self, entity requests to the entity.
        @return: (response-code, body)
        """
        operation = required_property('operation', request)
        type = request.properties.get('type') # Allow absent type for requests with a name.
        if type == self.type or operation.lower() == 'create':
            # Create requests are entity requests but must be handled by the agent since
            # the entity does not yet exist.
            target = self
        else:
            target = self.find_entity(request)
            target.entity_type.allowed(operation)
        try:
            method = getattr(target, operation.lower().replace("-", "_"))
        except AttributeError:
            not_implemented(operation, target.type)
        return method(request)

    def requested_type(self, request):
        type = request.properties.get('entityType')
        if type: return self.entity_type(type)
        else: return None

    def query(self, request):
        """Management node query operation"""
        entity_type = self.requested_type(request)
        if entity_type:
            all_attrs = set(entity_type.attributes.keys())
        else:
            all_attrs = self.schema.all_attributes

        names = set(request.body.get('attributeNames'))
        if names:
            unknown = names - all_attrs
            if unknown:
                if entity_type:
                    for_type =  " for type %s" % entity_type.name
                else:
                    for_type = ""
                raise NotFoundStatus("Unknown attributes %s%s." % (list(unknown), for_type))
        else:
            names = all_attrs

        results = []
        def add_result(entity):
            result = []
            non_empty = False
            for name in names:
                result.append(entity.attributes.get(name))
                if result[-1] is not None: non_empty = True
            if non_empty: results.append(result)

        self.entities.map_type(add_result, entity_type)
        return (OK, {'attributeNames': list(names), 'results': results})

    def create(self, request=None, attributes=None):
        """
        Create is special: it is directed at an entity but the entity
        does not yet exist so it is handled initially by the agent and
        then delegated to the new entity.
        """
        if request:
            attributes = request.body
            for a in ['type', 'name']:
                prop = request.properties.get(a)
                if prop:
                    old = attributes.setdefault(a, prop)
                    if old is not None and old != prop:
                        raise BadRequestStatus("Conflicting values for '%s'" % a)
                attributes[a] = prop
        if attributes.get('type') is None:
            raise BadRequestStatus("No 'type' attribute in %s" % attributes)
        self.schema.entity_type(attributes['type']).allowed('create')
        entity = self.create_entity(attributes)
        self.add_entity(entity)
        entity.create()
        return (CREATED, entity.attributes)

    def add_entity(self, entity): self.entities.add(entity)

    def remove(self, entity): self.entities.remove(entity)

    def get_types(self, request):
        type = self.requested_type(request)
        return (OK, dict((t.name, [b.name for b in t.all_bases])
                         for t in self.schema.by_type(type)))

    def get_annotations(self, request):
        type = self.requested_type(request)
        return (OK, dict((t.name, [a.name for a in t.annotations])
                         for t in self.schema.by_type(type)))

    def get_operations(self, request):
        type = self.requested_type(request)
        return (OK, dict((t, et.operations)
                         for t, et in self.schema.entity_types.iteritems()
                         if not type or type.name == t))

    def get_attributes(self, request):
        type = self.requested_type(request)
        return (OK, dict((t, [a for a in et.attributes])
                         for t, et in self.schema.entity_types.iteritems()
                         if not type or type.name == t))

    def get_mgmt_nodes(self, request):
        router = self.entities.map_type(None, 'router')[0]
        area = router.attributes['area']
        def node_address(node):
            return "amqp:/_topo/%s/%s/$management" % (area, node.attributes['addr'][1:])
        return (OK, self.entities.map_type(node_address, 'router.node'))


    def find_entity(self, request):
        """Find the entity addressed by request"""

        # ids is a map of identifying attribute values
        ids = dict((k, request.properties.get(k))
                   for k in ['name', 'identity'] if k in request.properties)
        if not len(ids): raise BadRequestStatus("No name or identity provided")

        def attrvals():
            """String form of the id attribute values for error messages"""
            return " ".join(["%s=%r" % (k, v) for k, v in ids.iteritems()])

        k, v = ids.iteritems().next() # Get the first id attribute
        found = self.entities.map_filter(None, lambda e: e.attributes.get(k) == v)
        if len(found) == 1:
            entity = found[0]
        elif len(found) > 1:
            raise InternalServerErrorStatus(
                "Duplicate (%s) entities with %s=%r" % (len(found), k, v))
        else:
            raise NotFoundStatus("No entity with %s" % attrvals())

        for k, v in ids.iteritems():
            if entity[k] != v: raise BadRequestStatus("Conflicting %s" % attrvals())

        request_type = request.properties.get('type')
        if request_type and not entity.entity_type.name_is(request_type):
            raise NotFoundStatus("Entity type '%s' does match requested type '%s'" %
                           (entity.entity_type.name, request_type))

        return entity

    def find_entity_by_type(self, type):
        return self.entities.map_type(None, type)
