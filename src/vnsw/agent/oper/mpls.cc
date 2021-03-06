/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <boost/uuid/uuid_io.hpp>
#include <cmn/agent_cmn.h>

#include <oper/interface.h>
#include <oper/vrf.h>
#include <oper/nexthop.h>
#include <oper/mpls.h>
#include <oper/mirror_table.h>
#include <oper/agent_sandesh.h>

using namespace std;

MplsTable *MplsTable::mpls_table_;

MplsLabel::~MplsLabel() { 
    if (label_ == MplsTable::kInvalidLabel) {
        return;
    }
    if ((type_ != MplsLabel::MCAST_NH) &&
        free_label_) {
        Agent::GetInstance()->GetMplsTable()->FreeLabel(label_);
    }
}

DBEntryBase::KeyPtr MplsLabel::GetDBRequestKey() const {
    MplsLabelKey *key = new MplsLabelKey(type_, label_);
    return DBEntryBase::KeyPtr(key);
}

void MplsLabel::SetKey(const DBRequestKey *k) { 
    const MplsLabelKey *key = static_cast<const MplsLabelKey *>(k);
    type_ = key->type_;
    label_ = key->label_;
}

AgentDBTable *MplsLabel::DBToTable() const {
    return MplsTable::GetInstance();
}

std::auto_ptr<DBEntry> MplsTable::AllocEntry(const DBRequestKey *k) const {
    const MplsLabelKey *key = static_cast<const MplsLabelKey *>(k);
    MplsLabel *mpls = new MplsLabel(key->type_, key->label_);
    return std::auto_ptr<DBEntry>(static_cast<DBEntry *>(mpls));
}

DBEntry *MplsTable::Add(const DBRequest *req) {
    MplsLabelKey *key = static_cast<MplsLabelKey *>(req->key.get());
    MplsLabel *mpls = new MplsLabel(key->type_, key->label_);

    mpls->free_label_ = true;
    if (mpls->type_ != MplsLabel::MCAST_NH) {
        UpdateLabel(key->label_, mpls);
    }
    ChangeHandler(mpls, req);
    mpls->SendObjectLog(AgentLogEvent::ADD);
    return mpls;
}

bool MplsTable::OnChange(DBEntry *entry, const DBRequest *req) {
    bool ret;
    MplsLabel *mpls = static_cast<MplsLabel *>(entry);
    ret = ChangeHandler(mpls, req);
    mpls->SendObjectLog(AgentLogEvent::CHANGE);
    return ret;
}

// No Change expected for MPLS Label
bool MplsTable::ChangeHandler(MplsLabel *mpls, const DBRequest *req) {
    bool ret = false;
    MplsLabelData *data = static_cast<MplsLabelData *>(req->data.get());
    NextHop *nh = static_cast<NextHop *>
        (Agent::GetInstance()->GetNextHopTable()->FindActiveEntry(data->nh_key));

    if (mpls->nh_ != nh) {
        mpls->nh_ = nh;
        ret = true;
    }

    return ret;
}

void MplsTable::Delete(DBEntry *entry, const DBRequest *req) {
    MplsLabel *mpls = static_cast<MplsLabel *>(entry);
    mpls->SendObjectLog(AgentLogEvent::DELETE);
}

DBTableBase *MplsTable::CreateTable(DB *db, const std::string &name) {
    mpls_table_ = new MplsTable(db, name);
    mpls_table_->Init();

    // We want to allocate labels from an offset
    // Pre-allocate entries 
    for (unsigned int i = 0; i < kStartLabel; i++) {
        mpls_table_->AllocLabel();
    }
    return mpls_table_;
};

void MplsLabel::CreateVlanNhReq(uint32_t label, const uuid &intf_uuid,
                                uint16_t tag) {
    DBRequest req;
    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;

    MplsLabelKey *key = new MplsLabelKey(MplsLabel::VPORT_NH, label);
    req.key.reset(key);

    MplsLabelData *data = new MplsLabelData(intf_uuid, tag);
    req.data.reset(data);

    MplsTable::GetInstance()->Enqueue(&req);
    return;
}

void MplsLabel::CreateVirtualHostPortLabelReq(uint32_t label,
                                              const string &ifname,
                                              bool policy) {
    DBRequest req;
    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;

    MplsLabelKey *key = new MplsLabelKey(MplsLabel::VPORT_NH, label);
    req.key.reset(key);

    MplsLabelData *data = new MplsLabelData(ifname, policy);
    req.data.reset(data);

    MplsTable::GetInstance()->Enqueue(&req);
    return;
}

void MplsLabel::CreateVPortLabelReq(uint32_t label, const uuid &intf_uuid,
                                    bool policy, InterfaceNHFlags::Type type) {
    DBRequest req;
    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;

    MplsLabelKey *key = new MplsLabelKey(MplsLabel::VPORT_NH, label);
    req.key.reset(key);

    MplsLabelData *data = new MplsLabelData(intf_uuid, policy, type);
    req.data.reset(data);

    MplsTable::GetInstance()->Enqueue(&req);
    return;
}

void MplsLabel::CreateMcastLabelReq(const string &vrf_name, const Ip4Address &grp_addr,
                                    const Ip4Address &src_addr, uint32_t src_label,
                                    COMPOSITETYPE comp_type) {
    DBRequest req;
    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;

    MplsLabelKey *key = new MplsLabelKey(MplsLabel::MCAST_NH, src_label);
    req.key.reset(key);

    MplsLabelData *data = new MplsLabelData(vrf_name, grp_addr, src_addr, false, comp_type);
    req.data.reset(data);

    MplsTable::GetInstance()->Enqueue(&req);
    return;
}

void MplsLabel::CreateEcmpLabelReq(uint32_t label, const string &vrf_name,
                                   const Ip4Address &addr) {
    DBRequest req;
    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;

    MplsLabelKey *key = new MplsLabelKey(MplsLabel::VPORT_NH, label);
    req.key.reset(key);

    MplsLabelData *data = new MplsLabelData(vrf_name, addr, true);
    req.data.reset(data);

    MplsTable::GetInstance()->Enqueue(&req);
    return;
}
                                   
void MplsLabel::DeleteMcastLabelReq(const string &vrf_name, const Ip4Address &grp_addr,
                                    const Ip4Address &src_addr, uint32_t src_label) {
    DBRequest req;
    req.oper = DBRequest::DB_ENTRY_DELETE;

    MplsLabelKey *key = new MplsLabelKey(MplsLabel::MCAST_NH, src_label);
    req.key.reset(key);
    req.data.reset(NULL);

    MplsTable::GetInstance()->Enqueue(&req);
    return;
}

void MplsLabel::DeleteReq(uint32_t label) {
    DBRequest req;
    req.oper = DBRequest::DB_ENTRY_DELETE;

    MplsLabelKey *key = new MplsLabelKey(MplsLabel::INVALID, label);
    req.key.reset(key);
    req.data.reset(NULL);
    MplsTable::GetInstance()->Enqueue(&req);
}

bool MplsLabel::DBEntrySandesh(Sandesh *sresp, std::string &name) const {
    MplsResp *resp = static_cast<MplsResp *>(sresp);

    MplsSandeshData data;
    data.set_label(label_);
    switch (type_) {
        case MplsLabel::INVALID:
            data.set_type("invalid");
            break;
        case MplsLabel::MCAST_NH:
            data.set_type("mcast_nh");
            break;
        case MplsLabel::VPORT_NH:
            data.set_type("vport_nh");
            break;
        default:
            assert(0);
    }
    nh_->SetNHSandeshData(data.nh);
    std::vector<MplsSandeshData> &list =
            const_cast<std::vector<MplsSandeshData>&>(resp->get_mpls_list());
    list.push_back(data);

    return true;
}

void MplsLabel::SendObjectLog(AgentLogEvent::type event) const {
    MplsObjectLogInfo info;
    string str, type_str, nh_type;
    
    switch(type_) {
        case INVALID:
           type_str.assign("Invalid ");
           break;
        case VPORT_NH:
           type_str.assign("Virtual Port");
           break;
        case MplsLabel::MCAST_NH:
            type_str.assign("mcast_nh");
            break;
    }
    info.set_type(type_str);
    info.set_label((int)label_);
    switch (event) {
        case AgentLogEvent::ADD:
            str.assign("Addition ");
            break;
        case AgentLogEvent::DELETE:
            str.assign("Deletion ");
            info.set_event(str);
            OPER_TRACE(Mpls, info);
            return;
        case AgentLogEvent::CHANGE:
            str.assign("Modification ");
            break;
        default:
            str.assign("Unknown");
            break;
    }
    info.set_event(str);
    const NextHop *nh = GetNextHop();
    const Interface *intf = NULL;
    /* Mpls is not expected to have any other nexthop apart from Interface 
       or Vlan */
    if (nh != NULL) {
        string policy_str("Disabled");
        const InterfaceNH *if_nh;
        const VlanNH *vlan_nh;

        switch(nh->GetType()) {
            case NextHop::INTERFACE:
                nh_type.assign("INTERFACE");
                if_nh = static_cast<const InterfaceNH *>(nh);
                intf = if_nh->GetInterface();
                if (if_nh->PolicyEnabled()) {
                    policy_str.assign("Enabled");
                }
                info.set_policy(policy_str);
                break;
            case NextHop::VLAN:
                nh_type.assign("VLAN");
                vlan_nh = static_cast<const VlanNH *>(nh);
                intf = vlan_nh->GetInterface();
                info.set_vlan_tag(vlan_nh->GetVlanTag());
                break;
            case NextHop::COMPOSITE:
                nh_type.assign("Composite");    
                break;
            default:
                nh_type.assign("unknown");
                break;
        }
    }
    info.set_nh_type(nh_type);
    /* Interface Nexthop pointed by Mpls object will always be of type VMPORT */
    if (intf) {
        string if_type_str;
        switch(intf->GetType()) {
            case Interface::VMPORT:
                if_type_str.assign("VMPORT");
                break;
            default:
                if_type_str.assign("Invalid");
                break;
        }
        info.set_intf_type(if_type_str);
        info.set_intf_uuid(UuidToString(intf->GetUuid()));
        info.set_intf_name(intf->GetName());
    }
    OPER_TRACE(Mpls, info);
}

void MplsReq::HandleRequest() const {
    AgentMplsSandesh *sand = new AgentMplsSandesh(context());
    sand->DoSandesh();
}
