/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <boost/uuid/uuid_io.hpp>

#include <cmn/agent_cmn.h>
#include <oper/agent_route.h>
#include <oper/peer.h>
#include <oper/nexthop.h>
#include <oper/peer.h>
#include <oper/mirror_table.h>
#include <controller/controller_init.h>
#include <controller/controller_export.h> 
#include <controller/controller_peer.h>
#include <controller/controller_types.h>

RouteExport::State::State() : 
    DBState(), exported_(false), force_chg_(false), server_(0),
    label_(MplsTable::kInvalidLabel), vn_(""), sg_list_() {
}

bool RouteExport::State::Changed(const AgentPath *path) const {
    if (exported_ == false)
        return true;

    if (force_chg_ == true)
        return true;

    if (label_ != path->GetLabel())
        return true;

    if (vn_ != path->GetDestVnName())
        return true;

    if (sg_list_ != path->GetSecurityGroupList())
        return true;

    return false;
}

void RouteExport::State::Update(const AgentPath *path) {
    force_chg_ = false;
    label_ = path->GetLabel();
    vn_ = path->GetDestVnName();
    sg_list_ = path->GetSecurityGroupList();
}

RouteExport::RouteExport(AgentRouteTable *rt_table):
    rt_table_(rt_table), marked_delete_(false), 
    table_delete_ref_(this, rt_table->deleter()) {
}

RouteExport::~RouteExport() {
    if (rt_table_) {
        rt_table_->Unregister(id_);
    }
    table_delete_ref_.Reset(NULL);
}

void RouteExport::ManagedDelete() {
    marked_delete_ = true;
}

void RouteExport::Notify(AgentXmppChannel *bgp_xmpp_peer, 
                         bool associate, AgentRouteTableAPIS::TableType type,
                         DBTablePartBase *partition, DBEntryBase *e) {
    RouteEntry *route = static_cast<RouteEntry *>(e);

    //If multicast or subnetbroadcast digress to multicast
    if (route->IsMulticast()) {
    	return MulticastNotify(bgp_xmpp_peer, associate, partition, e);
    }
    return UnicastNotify(bgp_xmpp_peer, partition, e, type);

}

void RouteExport::UnicastNotify(AgentXmppChannel *bgp_xmpp_peer, 
                                DBTablePartBase *partition, DBEntryBase *e,
                                AgentRouteTableAPIS::TableType type) {
    RouteEntry *route = static_cast<RouteEntry *>(e);
    State *state = static_cast<State *>(route->GetState(partition->parent(),
                                                        id_));
    AgentPath *path = route->FindPath(Agent::GetInstance()->GetLocalVmPeer());

    if (marked_delete_) {
        //Ignore route updates on delete marked vrf
        goto done;
    }

    if (!state && route->IsDeleted()) {
        goto done;
    }

    if (state == NULL) {
        state = new State();
        route->SetState(partition->parent(), id_, state);
    }

    if (path) {
        if (state->Changed(path)) {
            state->Update(path);
            CONTROLLER_TRACE(RouteExport,
                             bgp_xmpp_peer->GetBgpPeer()->GetName(),
                             route->GetVrfEntry()->GetName(),
                             route->ToString(),
                             false, path->GetLabel());
            state->exported_ = 
                AgentXmppChannel::ControllerSendRoute(bgp_xmpp_peer, 
                        static_cast<RouteEntry * >(route), state->vn_, 
                        state->label_, path->GetTunnelBmap(),
                        &path->GetSecurityGroupList(), true, type);
        }
    } else {
        if (state->exported_ == true) {
            CONTROLLER_TRACE(RouteExport, 
                    bgp_xmpp_peer->GetBgpPeer()->GetName(), 
                    route->GetVrfEntry()->GetName(), 
                    route->ToString(), 
                    true, 0);

            AgentXmppChannel::ControllerSendRoute(bgp_xmpp_peer, 
                    static_cast<RouteEntry *>(route), state->vn_, 
                    state->label_, TunnelType::AllType(), NULL, false, type);
            state->exported_ = false;
        }
    }
done:
    if (route->IsDeleted()) {
        if (state) {
            route->ClearState(partition->parent(), id_);
            delete state;
        }
    }
}

void RouteExport::MulticastNotify(AgentXmppChannel *bgp_xmpp_peer, 
                                  bool associate,
                                  DBTablePartBase *partition, 
                                  DBEntryBase *e) {
    RouteEntry *route = static_cast<RouteEntry *>(e);
    State *state = static_cast<State *>(route->GetState(partition->parent(), id_));

    if (route->CanBeDeleted() && (state != NULL) && (state->exported_ == true)) {
        CONTROLLER_TRACE(RouteExport, bgp_xmpp_peer->GetBgpPeer()->GetName(),
                route->GetVrfEntry()->GetName(), 
                route->ToString(), true, 0);

        AgentXmppChannel::ControllerSendMcastRoute(bgp_xmpp_peer, 
                                                   route, false);
        state->exported_ = false;
        route->ClearState(partition->parent(), id_);
        delete state;
        state = NULL;
        return;
    }

    if (route->IsDeleted()) {
        if (state) {
            route->ClearState(partition->parent(), id_);
            delete state;
        }
        return;
    }

    if (marked_delete_) {
        //Ignore route updates on delete marked vrf
        return;
    }

    if (state == NULL) {
        state = new State();
        route->SetState(partition->parent(), id_, state);
    }

    if (!route->CanBeDeleted() && ((state->exported_ == false) || 
                                  (state->force_chg_ == true))) {

        CONTROLLER_TRACE(RouteExport, bgp_xmpp_peer->GetBgpPeer()->GetName(),
                route->GetVrfEntry()->GetName(), 
                route->ToString(), associate, 0);

        state->exported_ = 
            AgentXmppChannel::ControllerSendMcastRoute(bgp_xmpp_peer, 
                                                       route, associate);
        state->force_chg_ = false;

        return;
    }
}

bool RouteExport::DeleteState(DBTablePartBase *partition,
                              DBEntryBase *entry) {
    State *state = static_cast<State *>
                       (entry->GetState(partition->parent(), id_));
    if (state) {
        entry->ClearState(partition->parent(), id_);
        delete state;
    }
    return true;
}

void RouteExport::Walkdone(DBTableBase *partition,
                           RouteExport *rt_export) {
    delete rt_export;
}

void RouteExport::Unregister() {
    //Start unregister process
    DBTableWalker *walker = Agent::GetInstance()->GetDB()->GetWalker();
    walker->WalkTable(rt_table_, NULL, 
            boost::bind(&RouteExport::DeleteState, this, _1, _2),
            boost::bind(&RouteExport::Walkdone, _1, this));
}

RouteExport* RouteExport::Init(AgentRouteTable *table, 
                               AgentXmppChannel *bgp_xmpp_peer) {
    RouteExport *rt_export = new RouteExport(table);
    bool associate = true;
    rt_export->id_ = table->Register(boost::bind(&RouteExport::Notify,
                                     rt_export, bgp_xmpp_peer, associate, 
                                     table->GetTableType(), _1, _2));
    return rt_export;
}

