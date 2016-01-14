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
#include <qpid/dispatch/amqp.h>
#include <stdio.h>

static void qdr_connection_opened_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_connection_closed_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_link_inbound_first_attach_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_link_inbound_second_attach_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_link_inbound_detach_CT(qdr_core_t *core, qdr_action_t *action, bool discard);

ALLOC_DEFINE(qdr_connection_t);
ALLOC_DEFINE(qdr_connection_work_t);

typedef enum {
    QDR_CONDITION_NO_ROUTE_TO_DESTINATION,
    QDR_CONDITION_ROUTED_LINK_LOST,
    QDR_CONDITION_FORBIDDEN
} qdr_condition_t;

//==================================================================================
// Internal Functions
//==================================================================================

qdr_terminus_t *qdr_terminus_router_control(void)
{
    qdr_terminus_t *term = qdr_terminus(0);
    qdr_terminus_add_capability(term, QD_CAPABILITY_ROUTER_CONTROL);
    return term;
}


qdr_terminus_t *qdr_terminus_router_data(void)
{
    qdr_terminus_t *term = qdr_terminus(0);
    qdr_terminus_add_capability(term, QD_CAPABILITY_ROUTER_DATA);
    return term;
}


//==================================================================================
// Interface Functions
//==================================================================================

qdr_connection_t *qdr_connection_opened(qdr_core_t            *core,
                                        bool                   incoming,
                                        qdr_connection_role_t  role,
                                        const char            *label,
                                        bool                   strip_annotations_in,
                                        bool                   strip_annotations_out)
{
    qdr_action_t     *action = qdr_action(qdr_connection_opened_CT, "connection_opened");
    qdr_connection_t *conn   = new_qdr_connection_t();

    ZERO(conn);
    conn->core                  = core;
    conn->user_context          = 0;
    conn->incoming              = incoming;
    conn->role                  = role;
    conn->label                 = label;
    conn->strip_annotations_in  = strip_annotations_in;
    conn->strip_annotations_out = strip_annotations_out;
    conn->mask_bit              = -1;
    DEQ_INIT(conn->links);
    DEQ_INIT(conn->work_list);
    conn->work_lock = sys_mutex();

    action->args.connection.conn = conn;
    qdr_action_enqueue(core, action);

    return conn;
}


void qdr_connection_closed(qdr_connection_t *conn)
{
    qdr_action_t *action = qdr_action(qdr_connection_closed_CT, "connection_closed");
    action->args.connection.conn = conn;
    qdr_action_enqueue(conn->core, action);
}


void qdr_connection_set_context(qdr_connection_t *conn, void *context)
{
    if (conn)
        conn->user_context = context;
}


void *qdr_connection_get_context(const qdr_connection_t *conn)
{
    return conn ? conn->user_context : 0;
}


int qdr_connection_process(qdr_connection_t *conn)
{
    qdr_connection_work_list_t  work_list;
    qdr_link_ref_list_t         links_with_deliveries;
    qdr_link_ref_list_t         links_with_credit;
    qdr_core_t                 *core = conn->core;

    sys_mutex_lock(conn->work_lock);
    DEQ_MOVE(conn->work_list, work_list);
    DEQ_MOVE(conn->links_with_deliveries, links_with_deliveries);
    DEQ_MOVE(conn->links_with_credit, links_with_credit);
    sys_mutex_unlock(conn->work_lock);

    int event_count = DEQ_SIZE(work_list);
    qdr_connection_work_t *work = DEQ_HEAD(work_list);
    while (work) {
        DEQ_REMOVE_HEAD(work_list);

        switch (work->work_type) {
        case QDR_CONNECTION_WORK_FIRST_ATTACH :
            core->first_attach_handler(core->user_context, conn, work->link, work->source, work->target);
            break;

        case QDR_CONNECTION_WORK_SECOND_ATTACH :
            core->second_attach_handler(core->user_context, work->link, work->source, work->target);
            break;

        case QDR_CONNECTION_WORK_DETACH :
            core->detach_handler(core->user_context, work->link, work->error);
            break;
        }

        qdr_terminus_free(work->source);
        qdr_terminus_free(work->target);
        free_qdr_connection_work_t(work);

        work = DEQ_HEAD(work_list);
    }

    qdr_link_ref_t *ref = DEQ_HEAD(links_with_deliveries);
    while (ref) {
        core->push_handler(core->user_context, ref->link);
        qdr_del_link_ref(&links_with_deliveries, ref->link, QDR_LINK_LIST_CLASS_DELIVERY);
        ref = DEQ_HEAD(links_with_deliveries);
    }

    ref = DEQ_HEAD(links_with_credit);
    while (ref) {
        core->flow_handler(core->user_context, ref->link, ref->link->incremental_credit);
        ref->link->incremental_credit = 0;
        qdr_del_link_ref(&links_with_credit, ref->link, QDR_LINK_LIST_CLASS_FLOW);
        ref = DEQ_HEAD(links_with_credit);
    }

    return event_count;
}


void qdr_link_set_context(qdr_link_t *link, void *context)
{
    if (link)
        link->user_context = context;
}


void *qdr_link_get_context(const qdr_link_t *link)
{
    return link ? link->user_context : 0;
}


qd_link_type_t qdr_link_type(const qdr_link_t *link)
{
    return link->link_type;
}


qd_direction_t qdr_link_direction(const qdr_link_t *link)
{
    return link->link_direction;
}


bool qdr_link_is_anonymous(const qdr_link_t *link)
{
    return link->owning_addr == 0;
}


bool qdr_link_is_routed(const qdr_link_t *link)
{
    return link->connected_link != 0;
}


bool qdr_link_strip_annotations_in(const qdr_link_t *link)
{
    return link->strip_annotations_in;
}


bool qdr_link_strip_annotations_out(const qdr_link_t *link)
{
    return link->strip_annotations_out;
}


const char *qdr_link_name(const qdr_link_t *link)
{
    return link->name;
}


qdr_link_t *qdr_link_first_attach(qdr_connection_t *conn,
                                  qd_direction_t    dir,
                                  qdr_terminus_t   *source,
                                  qdr_terminus_t   *target,
                                  const char       *name)
{
    qdr_action_t   *action         = qdr_action(qdr_link_inbound_first_attach_CT, "link_first_attach");
    qdr_link_t     *link           = new_qdr_link_t();
    qdr_terminus_t *local_terminus = dir == QD_OUTGOING ? source : target;

    ZERO(link);
    link->core = conn->core;
    link->conn = conn;
    link->name = (char*) malloc(strlen(name));
    strcpy(link->name, name);
    link->link_direction = dir;
    link->capacity = 32;  // TODO - make this configurable

    link->strip_annotations_in  = conn->strip_annotations_in;
    link->strip_annotations_out = conn->strip_annotations_out;

    if      (qdr_terminus_has_capability(local_terminus, QD_CAPABILITY_ROUTER_CONTROL))
        link->link_type = QD_LINK_CONTROL;
    else if (qdr_terminus_has_capability(local_terminus, QD_CAPABILITY_ROUTER_DATA))
        link->link_type = QD_LINK_ROUTER;

    action->args.connection.conn   = conn;
    action->args.connection.link   = link;
    action->args.connection.dir    = dir;
    action->args.connection.source = source;
    action->args.connection.target = target;
    qdr_action_enqueue(conn->core, action);

    return link;
}


void qdr_link_second_attach(qdr_link_t *link, qdr_terminus_t *source, qdr_terminus_t *target)
{
    qdr_action_t *action = qdr_action(qdr_link_inbound_second_attach_CT, "link_second_attach");

    action->args.connection.link   = link;
    action->args.connection.source = source;
    action->args.connection.target = target;
    qdr_action_enqueue(link->core, action);
}


void qdr_link_detach(qdr_link_t *link, qd_detach_type_t dt, qdr_error_t *error)
{
    qdr_action_t *action = qdr_action(qdr_link_inbound_detach_CT, "link_detach");

    action->args.connection.conn   = link->conn;
    action->args.connection.link   = link;
    action->args.connection.error  = error;
    action->args.connection.dt     = dt;
    qdr_action_enqueue(link->core, action);
}


void qdr_connection_handlers(qdr_core_t                *core,
                             void                      *context,
                             qdr_connection_activate_t  activate,
                             qdr_link_first_attach_t    first_attach,
                             qdr_link_second_attach_t   second_attach,
                             qdr_link_detach_t          detach,
                             qdr_link_flow_t            flow,
                             qdr_link_offer_t           offer,
                             qdr_link_drained_t         drained,
                             qdr_link_push_t            push,
                             qdr_link_deliver_t         deliver)
{
    core->user_context          = context;
    core->activate_handler      = activate;
    core->first_attach_handler  = first_attach;
    core->second_attach_handler = second_attach;
    core->detach_handler        = detach;
    core->flow_handler          = flow;
    core->offer_handler         = offer;
    core->drained_handler       = drained;
    core->push_handler          = push;
    core->deliver_handler       = deliver;
}


//==================================================================================
// In-Thread Functions
//==================================================================================

void qdr_connection_activate_CT(qdr_core_t *core, qdr_connection_t *conn)
{
    core->activate_handler(core->user_context, conn);
}


static void qdr_connection_enqueue_work_CT(qdr_core_t            *core,
                                           qdr_connection_t      *conn,
                                           qdr_connection_work_t *work)
{
    sys_mutex_lock(conn->work_lock);
    DEQ_INSERT_TAIL(conn->work_list, work);
    bool notify = DEQ_SIZE(conn->work_list) == 1;
    sys_mutex_unlock(conn->work_lock);

    if (notify)
        qdr_connection_activate_CT(core, conn);
}


#define QDR_DISCRIMINATOR_SIZE 16
static void qdr_generate_discriminator(char *string)
{
    static const char *table = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+_";
    long int rnd1 = random();
    long int rnd2 = random();
    long int rnd3 = random();
    int      idx;
    int      cursor = 0;

    for (idx = 0; idx < 5; idx++) {
        string[cursor++] = table[(rnd1 >> (idx * 6)) & 63];
        string[cursor++] = table[(rnd2 >> (idx * 6)) & 63];
        string[cursor++] = table[(rnd3 >> (idx * 6)) & 63];
    }
    string[cursor] = '\0';
}


/**
 * Generate a temporary routable address for a destination connected to this
 * router node.
 */
static void qdr_generate_temp_addr(qdr_core_t *core, char *buffer, size_t length)
{
    char discriminator[QDR_DISCRIMINATOR_SIZE];
    qdr_generate_discriminator(discriminator);
    snprintf(buffer, length, "amqp:/_topo/%s/%s/temp.%s", core->router_area, core->router_id, discriminator);
}


/**
 * Generate a link name
 */
static void qdr_generate_link_name(const char *label, char *buffer, size_t length)
{
    char discriminator[QDR_DISCRIMINATOR_SIZE];
    qdr_generate_discriminator(discriminator);
    snprintf(buffer, length, "%s.%s", label, discriminator);
}


static qdr_link_t *qdr_create_link_CT(qdr_core_t       *core,
                                      qdr_connection_t *conn,
                                      qd_link_type_t    link_type,
                                      qd_direction_t    dir,
                                      qdr_terminus_t   *source,
                                      qdr_terminus_t   *target)
{
    //
    // Create a new link, initiated by the router core.  This will involve issuing a first-attach outbound.
    //
    qdr_link_t *link = new_qdr_link_t();
    ZERO(link);

    link->core           = core;
    link->user_context   = 0;
    link->conn           = conn;
    link->link_type      = link_type;
    link->link_direction = dir;
    link->capacity       = 32; // TODO - make this configurable
    link->name           = (char*) malloc(QDR_DISCRIMINATOR_SIZE + 8);
    qdr_generate_link_name("qdlink", link->name, QDR_DISCRIMINATOR_SIZE + 8);

    qdr_connection_work_t *work = new_qdr_connection_work_t();
    ZERO(work);
    work->work_type = QDR_CONNECTION_WORK_FIRST_ATTACH;
    work->link      = link;
    work->source    = source;
    work->target    = target;

    qdr_connection_enqueue_work_CT(core, conn, work);
    return link;
}


static void qdr_link_outbound_detach_CT(qdr_core_t *core, qdr_link_t *link, qdr_condition_t condition)
{
}


static void qdr_link_outbound_second_attach_CT(qdr_core_t *core, qdr_link_t *link, qdr_terminus_t *source, qdr_terminus_t *target)
{
    qdr_connection_work_t *work = new_qdr_connection_work_t();
    ZERO(work);
    work->work_type = QDR_CONNECTION_WORK_SECOND_ATTACH;
    work->link      = link;
    work->source    = source;
    work->target    = target;

    qdr_connection_enqueue_work_CT(core, link->conn, work);
}


static void qdr_forward_first_attach_CT(qdr_core_t *core, qdr_link_t *link, qdr_address_t *addr,
                                        qdr_terminus_t *source, qdr_terminus_t *target)
{
}


static char qdr_prefix_for_dir(qd_direction_t dir)
{
    return (dir == QD_INCOMING) ? 'C' : 'D';
}


static qd_address_semantics_t qdr_semantics_for_address(qdr_core_t *core, qd_field_iterator_t *iter)
{
    qdr_address_t *addr = 0;

    //
    // Question: Should we use a new prefix for configuration? (No: allows the possibility of
    //           static routes; yes: prevents occlusion by mobile addresses with specified semantics)
    //
    qd_hash_retrieve_prefix(core->addr_hash, iter, (void**) &addr);
    return /* addr ? addr->semantics : */  QD_SEMANTICS_ANYCAST_BALANCED; // FIXME
}


/**
 * Check an address to see if it no longer has any associated destinations.
 * Depending on its policy, the address may be eligible for being closed out
 * (i.e. Logging its terminal statistics and freeing its resources).
 */
void qdr_check_addr_CT(qdr_core_t *core, qdr_address_t *addr, bool was_local)
{
    if (addr == 0)
        return;

    //
    // If we have just removed a local linkage and it was the last local linkage,
    // we need to notify the router module that there is no longer a local
    // presence of this address.
    //
    if (was_local && DEQ_SIZE(addr->rlinks) == 0) {
        const char *key = (const char*) qd_hash_key_by_handle(addr->hash_handle);
        if (key && *key == 'M')
            qdr_post_mobile_removed_CT(core, key);
    }

    //
    // If the address has no in-process consumer or destinations, it should be
    // deleted.
    //
    if (DEQ_SIZE(addr->subscriptions) == 0 && DEQ_SIZE(addr->rlinks) == 0 && DEQ_SIZE(addr->inlinks) == 0 &&
        DEQ_SIZE(addr->rnodes) == 0 && !addr->waypoint && !addr->block_deletion) {
        qd_hash_remove_by_handle(core->addr_hash, addr->hash_handle);
        DEQ_REMOVE(core->addrs, addr);
        qd_hash_handle_free(addr->hash_handle);
        free_qdr_address_t(addr);
    }
}


/**
 * qdr_lookup_terminus_address_CT
 *
 * Lookup a terminus address in the route table and possibly create a new address
 * if no match is found.
 *
 * @param core Pointer to the core object
 * @param dir Direction of the link for the terminus
 * @param terminus The terminus containing the addressing information to be looked up
 * @param create_if_not_found Iff true, return a pointer to a newly created address record
 * @param accept_dynamic Iff true, honor the dynamic flag by creating a dynamic address
 * @param [out] link_route True iff the lookup indicates that an attach should be routed
 * @return Pointer to an address record or 0 if none is found
 */
static qdr_address_t *qdr_lookup_terminus_address_CT(qdr_core_t     *core,
                                                     qd_direction_t  dir,
                                                     qdr_terminus_t *terminus,
                                                     bool            create_if_not_found,
                                                     bool            accept_dynamic,
                                                     bool           *link_route)
{
    qdr_address_t *addr = 0;

    //
    // Unless expressly stated, link routing is not indicated for this terminus.
    //
    *link_route = false;

    if (qdr_terminus_is_dynamic(terminus)) {
        //
        // The terminus is dynamic.  Check to see if there is an address provided
        // in the dynamic node properties.  If so, look that address up as a link-routed
        // destination.
        //
        qd_field_iterator_t *dnp_address = qdr_terminus_dnp_address(terminus);
        if (dnp_address) {
            qd_address_iterator_override_prefix(dnp_address, qdr_prefix_for_dir(dir));
            qd_hash_retrieve_prefix(core->addr_hash, dnp_address, (void**) &addr);
            qd_field_iterator_free(dnp_address);
            *link_route = true;
            return addr;
        }

        //
        // The dynamic terminus has no address in the dynamic-node-propteries.  If we are
        // permitted to generate dynamic addresses, create a new address that is local to
        // this router and insert it into the address table with a hash index.
        //
        if (!accept_dynamic)
            return 0;

        char temp_addr[200];
        bool generating = true;
        while (generating) {
            //
            // The address-generation process is performed in a loop in case the generated
            // address collides with a previously generated address (this should be _highly_
            // unlikely).
            //
            qdr_generate_temp_addr(core, temp_addr, 200);
            qd_field_iterator_t *temp_iter = qd_address_iterator_string(temp_addr, ITER_VIEW_ADDRESS_HASH);
            qd_hash_retrieve(core->addr_hash, temp_iter, (void**) &addr);
            if (!addr) {
                addr = qdr_address_CT(core, QD_SEMANTICS_ANYCAST_CLOSEST);
                qd_hash_insert(core->addr_hash, temp_iter, addr, &addr->hash_handle);
                DEQ_INSERT_TAIL(core->addrs, addr);
                qdr_terminus_set_address(terminus, temp_addr);
                generating = false;
            }
            qd_field_iterator_free(temp_iter);
        }
        return addr;
    }

    //
    // If the terminus is anonymous, there is no address to look up.
    //
    if (qdr_terminus_is_anonymous(terminus))
        return 0;

    //
    // The terminus has a non-dynamic address that we need to look up.  First, look for
    // a link-route destination for the address.
    //
    qd_field_iterator_t *iter = qdr_terminus_get_address(terminus);
    qd_address_iterator_reset_view(iter, ITER_VIEW_ADDRESS_HASH);
    qd_address_iterator_override_prefix(iter, qdr_prefix_for_dir(dir));
    qd_hash_retrieve_prefix(core->addr_hash, iter, (void**) &addr);
    if (addr) {
        *link_route = true;
        return addr;
    }

    //
    // There was no match for a link-route destination, look for a message-route address.
    //
    qd_address_iterator_override_prefix(iter, '\0'); // Cancel previous override
    qd_hash_retrieve(core->addr_hash, iter, (void**) &addr);
    if (!addr && create_if_not_found) {
        qd_address_semantics_t sem = qdr_semantics_for_address(core, iter);
        addr = qdr_address_CT(core, sem);
        qd_hash_insert(core->addr_hash, iter, addr, &addr->hash_handle);
        DEQ_INSERT_TAIL(core->addrs, addr);
    }

    return addr;
}


static void qdr_connection_opened_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_connection_t *conn = action->args.connection.conn;
    DEQ_ITEM_INIT(conn);
    DEQ_INSERT_TAIL(core->open_connections, conn);

    if (conn->role == QDR_ROLE_NORMAL) {
        //
        // No action needed for NORMAL connections
        //
        return;
    }

    if (conn->role == QDR_ROLE_INTER_ROUTER) {
        //
        // Assign a unique mask-bit to this connection as a reference to be used by
        // the router module
        //
        if (qd_bitmask_first_set(core->neighbor_free_mask, &conn->mask_bit))
            qd_bitmask_clear_bit(core->neighbor_free_mask, conn->mask_bit);
        else {
            qd_log(core->log, QD_LOG_CRITICAL, "Exceeded maximum inter-router connection count");
            return;
        }

        if (!conn->incoming) {
            //
            // The connector-side of inter-router connections is responsible for setting up the
            // inter-router links:  Two (in and out) for control, two for routed-message transfer.
            //
            (void) qdr_create_link_CT(core, conn, QD_LINK_CONTROL, QD_INCOMING, qdr_terminus_router_control(), qdr_terminus_router_control());
            (void) qdr_create_link_CT(core, conn, QD_LINK_CONTROL, QD_OUTGOING, qdr_terminus_router_control(), qdr_terminus_router_control());
            (void) qdr_create_link_CT(core, conn, QD_LINK_ROUTER,  QD_INCOMING, qdr_terminus_router_data(), qdr_terminus_router_data());
            (void) qdr_create_link_CT(core, conn, QD_LINK_ROUTER,  QD_OUTGOING, qdr_terminus_router_data(), qdr_terminus_router_data());
        }
    }

    //
    // If the role is ON_DEMAND:
    //    Activate waypoints associated with this connection
    //    Activate link-route destinations associated with this connection
    //
}


static void qdr_connection_closed_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_connection_t *conn = action->args.connection.conn;

    //
    // TODO - Deactivate waypoints and link-route destinations for this connection
    //

    //
    // TODO - Clean up links associated with this connection
    //        This involves the links and the dispositions of deliveries stored
    //        with the links.
    //

    //
    // Discard items on the work list
    //

    DEQ_REMOVE(core->open_connections, conn);
    sys_mutex_free(conn->work_lock);
    free_qdr_connection_t(conn);
}


static void qdr_link_inbound_first_attach_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_connection_t  *conn   = action->args.connection.conn;
    qdr_link_t        *link   = action->args.connection.link;
    qd_direction_t     dir    = action->args.connection.dir;
    qdr_terminus_t    *source = action->args.connection.source;
    qdr_terminus_t    *target = action->args.connection.target;

    //
    // Reject any attaches of inter-router links that arrive on connections that are not inter-router.
    // Reject any waypoint links.  Waypoint links are always initiated by a router, not the remote container.
    //
    if ((link->link_type == QD_LINK_WAYPOINT) ||
        ((link->link_type == QD_LINK_CONTROL || link->link_type == QD_LINK_ROUTER) && conn->role != QDR_ROLE_INTER_ROUTER)) {
        qdr_link_outbound_detach_CT(core, link, QDR_CONDITION_FORBIDDEN);
        qdr_terminus_free(source);
        qdr_terminus_free(target);
    }

    if (dir == QD_INCOMING) {
        //
        // Handle incoming link cases
        //
        switch (link->link_type) {
        case QD_LINK_ENDPOINT: {
            if (qdr_terminus_is_anonymous(target)) {
                link->owning_addr = 0;
                qdr_link_outbound_second_attach_CT(core, link, source, target);
                qdr_link_issue_credit_CT(core, link, link->capacity);

            } else {
                //
                // This link has a target address
                //
                bool           link_route;
                qdr_address_t *addr = qdr_lookup_terminus_address_CT(core, dir, target, false, false, &link_route);
                if (!addr) {
                    //
                    // No route to this destination, reject the link
                    //
                    qdr_link_outbound_detach_CT(core, link, QDR_CONDITION_NO_ROUTE_TO_DESTINATION);
                    qdr_terminus_free(source);
                    qdr_terminus_free(target);
                }

                else if (link_route)
                    //
                    // This is a link-routed destination, forward the attach to the next hop
                    //
                    qdr_forward_first_attach_CT(core, link, addr, source, target);

                else {
                    //
                    // Associate the link with the address.  With this association, it will be unnecessary
                    // to do an address lookup for deliveries that arrive on this link.
                    //
                    link->owning_addr = addr;
                    qdr_add_link_ref(&addr->inlinks, link, QDR_LINK_LIST_CLASS_ADDRESS);
                    qdr_link_outbound_second_attach_CT(core, link, source, target);
                    qdr_link_issue_credit_CT(core, link, link->capacity);
                }
            }
            break;
        }

        case QD_LINK_WAYPOINT:
            // No action, waypoint links are rejected above.
            break;

        case QD_LINK_CONTROL:
            qdr_link_outbound_second_attach_CT(core, link, source, target);
            qdr_link_issue_credit_CT(core, link, link->capacity);
            break;

        case QD_LINK_ROUTER:
            qdr_link_outbound_second_attach_CT(core, link, source, target);
            qdr_link_issue_credit_CT(core, link, link->capacity);
            break;
        }
    } else {
        //
        // Handle outgoing link cases
        //
        switch (link->link_type) {
        case QD_LINK_ENDPOINT: {
            bool           link_route;
            qdr_address_t *addr = qdr_lookup_terminus_address_CT(core, dir, source, true, true, &link_route);
            if (!addr) {
                //
                // No route to this destination, reject the link
                //
                qdr_link_outbound_detach_CT(core, link, QDR_CONDITION_NO_ROUTE_TO_DESTINATION);
                qdr_terminus_free(source);
                qdr_terminus_free(target);
            }

            else if (link_route)
                //
                // This is a link-routed destination, forward the attach to the next hop
                //
                qdr_forward_first_attach_CT(core, link, addr, source, target);

            else {
                //
                // Associate the link with the address.  With this association, it will be unnecessary
                // to do an address lookup for deliveries that arrive on this link.
                //
                link->owning_addr = addr;
                qdr_add_link_ref(&addr->rlinks, link, QDR_LINK_LIST_CLASS_ADDRESS);
                if (DEQ_SIZE(addr->rlinks) == 1) {
                    const char *key = (const char*) qd_hash_key_by_handle(addr->hash_handle);
                    if (key && *key == 'M')
                        qdr_post_mobile_added_CT(core, key);
                }
                qdr_link_outbound_second_attach_CT(core, link, source, target);
            }
            break;
        }

        case QD_LINK_WAYPOINT:
            // No action, waypoint links are rejected above.
            break;

        case QD_LINK_CONTROL:
            link->owning_addr = core->hello_addr;
            qdr_add_link_ref(&core->hello_addr->rlinks, link, QDR_LINK_LIST_CLASS_ADDRESS);
            core->control_links_by_mask_bit[conn->mask_bit] = link;
            qdr_link_outbound_second_attach_CT(core, link, source, target);
            break;

        case QD_LINK_ROUTER:
            core->data_links_by_mask_bit[conn->mask_bit] = link;
            qdr_link_outbound_second_attach_CT(core, link, source, target);
            break;
        }
    }

    //
    // Cases to be handled:
    //
    // dir = Incoming or Outgoing:
    //    Link is an router-control link
    //       Note the control link on the connection
    //       Issue a second attach back to the originating node
    //
    // dir = Incoming:
    //    Issue credit for the inbound fifo
    //
    // dir = Outgoing:
    //    Link is a router-control link
    //       Associate the link with the router-hello address
    //       Associate the link with the link-mask-bit being used by the router
    //
}


static void qdr_link_inbound_second_attach_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_link_t       *link   = action->args.connection.link;
    qdr_connection_t *conn   = link->conn;
    qdr_terminus_t   *source = action->args.connection.source;
    qdr_terminus_t   *target = action->args.connection.target;

    if (link->link_direction == QD_INCOMING) {
        //
        // Handle incoming link cases
        //
        qdr_link_issue_credit_CT(core, link, link->capacity);
        switch (link->link_type) {
        case QD_LINK_ENDPOINT:
            break;

        case QD_LINK_WAYPOINT:
            break;

        case QD_LINK_CONTROL:
            break;

        case QD_LINK_ROUTER:
            break;
        }
    } else {
        //
        // Handle outgoing link cases
        //
        switch (link->link_type) {
        case QD_LINK_ENDPOINT:
            break;

        case QD_LINK_WAYPOINT:
            break;

        case QD_LINK_CONTROL:
            link->owning_addr = core->hello_addr;
            qdr_add_link_ref(&core->hello_addr->rlinks, link, QDR_LINK_LIST_CLASS_ADDRESS);
            core->control_links_by_mask_bit[conn->mask_bit] = link;
            break;

        case QD_LINK_ROUTER:
            core->data_links_by_mask_bit[conn->mask_bit] = link;
            break;
        }
    }

    qdr_terminus_free(source);
    qdr_terminus_free(target);

    //
    // Cases to be handled:
    //
    // Link is a router-control link:
    //    Note the control link on the connection
    //    Associate the link with the router-hello address
    //    Associate the link with the link-mask-bit being used by the router
    // Link is link-routed:
    //    Propagate the second attach back toward the originating node
    // Link is Incoming:
    //    Issue credit for the inbound fifo
    //
}


static void qdr_link_inbound_detach_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_connection_t *conn      = action->args.connection.conn;
    qdr_link_t       *link      = action->args.connection.link;
    //qdr_error_t      *error     = action->args.connection.error;
    qd_detach_type_t  dt        = action->args.connection.dt;
    qdr_address_t    *addr      = link->owning_addr;
    bool              was_local = false;

    link->owning_addr = 0;

    if (link->link_direction == QD_INCOMING) {
        //
        // Handle incoming link cases
        //
        switch (link->link_type) {
        case QD_LINK_ENDPOINT:
            if (addr)
                qdr_del_link_ref(&addr->inlinks, link, QDR_LINK_LIST_CLASS_ADDRESS);
            break;

        case QD_LINK_WAYPOINT:
            break;

        case QD_LINK_CONTROL:
            break;

        case QD_LINK_ROUTER:
            break;
        }
    } else {
        //
        // Handle outgoing link cases
        //
        switch (link->link_type) {
        case QD_LINK_ENDPOINT:
            if (addr) {
                qdr_del_link_ref(&addr->rlinks, link, QDR_LINK_LIST_CLASS_ADDRESS);
                was_local = true;
            }
            break;

        case QD_LINK_WAYPOINT:
            break;

        case QD_LINK_CONTROL:
            qdr_del_link_ref(&core->hello_addr->rlinks, link, QDR_LINK_LIST_CLASS_ADDRESS);
            core->control_links_by_mask_bit[conn->mask_bit] = 0;
            qdr_post_link_lost_CT(core, conn->mask_bit);
            break;

        case QD_LINK_ROUTER:
            core->data_links_by_mask_bit[conn->mask_bit] = 0;
            break;
        }
    }

    //
    // If the detach occurred via protocol, send a detach back.
    // TODO - Note that this is not appropriate for routed links.
    //
    if (dt != QD_LOST)
        qdr_link_outbound_detach_CT(core, link, 0);  // TODO - Fix error arg

    //
    // If there was an address associated with this link, check to see if any address-related
    // cleanup has to be done.
    //
    if (addr)
        qdr_check_addr_CT(core, addr, was_local);

    //
    // Cases to be handled:
    //
    // Link is link-routed:
    //    Propagate the detach along the link-chain
    // Link is half-detached and not link-routed:
    //    Issue a detach back to the originating node
    // Link is fully detached:
    //    Free the qdr_link object
    //    Remove any address linkages associated with this link
    //       If the last dest for a local address is lost, notify the router (mobile_removed)
    // Link is a router-control link:
    //    Issue a link-lost indication to the router
    //
}


