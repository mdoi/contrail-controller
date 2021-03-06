/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_intf_ksync_h
#define vnsw_agent_intf_ksync_h

#include <net/ethernet.h>

#include <boost/asio.hpp>
#include <boost/bind.hpp>

#include <db/db_entry.h>
#include <db/db_table.h>
#include <db/db_table_partition.h>
#include <ksync/ksync_entry.h>
#include <ksync/ksync_object.h>
#include "oper/interface.h"
#include "vr_types.h"
#include "vr_interface.h"
#include "ksync/agent_ksync_types.h"

using namespace std;

void KSyncInterfaceCreate(Interface::Type type, const char *if_name,
                          uint32_t vrf_id, uint32_t &ifindex, uint32_t &fd,
                          struct ether_addr &mac);
void KSyncInterfaceDelete(Interface::Type type, const char *if_name,
                          uint32_t vrf_id, uint32_t ifindex, uint32_t fd);
void GetPhyMac(const char *ifname, char *mac);

class Timer;
class IntfKSyncObject;

class IntfKSyncEntry : public KSyncNetlinkDBEntry {
public:
    IntfKSyncEntry(const IntfKSyncEntry *entry, uint32_t index) : 
        KSyncNetlinkDBEntry(index), ifname_(entry->ifname_),
        type_(entry->type_), intf_id_(entry->intf_id_), vrf_id_(entry->vrf_id_),
        fd_(kInvalidIndex), has_service_vlan_(entry->has_service_vlan_),
        mac_(entry->mac_), ip_(entry->ip_),
        policy_enabled_(entry->policy_enabled_),
        analyzer_name_(entry->analyzer_name_),
        mirror_direction_(entry->mirror_direction_),
        active_(false), os_index_(Interface::kInvalidIndex), 
        network_id_(entry->network_id_), sub_type_(entry->sub_type_) {
    };

    IntfKSyncEntry(const Interface *intf) :
        KSyncNetlinkDBEntry(kInvalidIndex), ifname_(intf->GetName()),
            type_(intf->GetType()), intf_id_(intf->GetInterfaceId()), 
            vrf_id_(intf->GetVrfId()), fd_(-1), has_service_vlan_(false),
            mac_(intf->GetMacAddr()), ip_(0),
            policy_enabled_(false), analyzer_name_(), active_(false), 
            os_index_(intf->GetOsIfindex()),
            sub_type_(VirtualHostInterface::HOST) {
        // Max name size supported by kernel
        assert(strlen(ifname_.c_str()) < IF_NAMESIZE);
        network_id_ = 0;
        if (type_ == Interface::VMPORT) {
            const VmPortInterface *vmitf = 
                static_cast<const VmPortInterface *>(intf);
            if (vmitf->IsDhcpSnoopIp()) {
                ip_ = vmitf->GetIpAddr().to_ulong();
            }
            network_id_ = vmitf->GetVxLanId();
        }
    };

    virtual ~IntfKSyncEntry() {};

    virtual bool IsLess(const KSyncEntry &rhs) const {
        const IntfKSyncEntry &entry = static_cast<const IntfKSyncEntry &>(rhs);
        return ifname_ < entry.ifname_;
    };

    virtual std::string ToString() const;

    virtual int AddMsg(char *buf, int buf_len);
    virtual int ChangeMsg(char *buf, int buf_len);
    virtual int DeleteMsg(char *buf, int buf_len);
    virtual KSyncEntry *UnresolvedReference() {
        return NULL;
    };

    virtual bool Sync(DBEntry *e) {
        Interface *intf = static_cast<Interface *>(e);
        bool ret = false;

        if (active_ != intf->GetActiveState()) {
            active_ = intf->GetActiveState();
            ret = true;
        }

        if (os_index_ != intf->GetOsIfindex()) {
            os_index_ = intf->GetOsIfindex();
            mac_ = intf->GetMacAddr();
            ret = true;
        }

        if (intf->GetType() == Interface::VMPORT) {
            VmPortInterface *vm_port = static_cast<VmPortInterface *>(intf);
            if (vm_port->IsDhcpSnoopIp()) {
                if (ip_ != vm_port->GetIpAddr().to_ulong()) {
                    ip_ = vm_port->GetIpAddr().to_ulong();
                    ret = true;
                }
            } else {
                if (ip_) {
                    ip_ = 0;
                    ret = true;
                }
            }
        }

        uint32_t vrf_id = VIF_VRF_INVALID;
        bool policy_enabled = false;
        std::string analyzer_name;    
        Interface::MirrorDirection mirror_direction;
        bool has_service_vlan = false;
        if (active_) {
            vrf_id = intf->GetVrfId();
            if (vrf_id == VrfEntry::kInvalidIndex) {
                vrf_id = VIF_VRF_INVALID;
            }
            if (intf->GetType() == Interface::VMPORT) {
                VmPortInterface *vm_port = static_cast<VmPortInterface *>(intf);
                has_service_vlan = vm_port->HasServiceVlan();
                // Policy is not supported on service-vm interfaces.
                // So, disable policy if service-vlan interface
                if (has_service_vlan) {
                    policy_enabled = false;
                } else {
                    policy_enabled = vm_port->IsPolicyEnabled();
                }
                analyzer_name = vm_port->GetAnalyzer();
                mirror_direction = vm_port->GetMirrorDirection();
                if (network_id_ != vm_port->GetVxLanId()) {
                    network_id_ = vm_port->GetVxLanId();
                    ret = true;
                }
            }
        }

        if (intf->GetType() == Interface::VHOST) {
            VirtualHostInterface *vhost = 
                static_cast<VirtualHostInterface *>(intf);
            sub_type_ = vhost->GetSubType();
        }

        if (vrf_id != vrf_id_) {
            vrf_id_ = vrf_id;
            ret = true;
        }

        if (policy_enabled_ != policy_enabled) {
            policy_enabled_ = policy_enabled;
            ret = true;
        }

        if (analyzer_name_ != analyzer_name) {
            analyzer_name_ = analyzer_name;
            ret = true;
        }

        if (mirror_direction_ != mirror_direction) {
            mirror_direction_ = mirror_direction;
            ret = true;
        }

        if (has_service_vlan_ != has_service_vlan) {
            has_service_vlan_ = has_service_vlan;
            ret = true;
        }

        return ret;
    };

    KSyncDBObject *GetObject(); 
    const uint8_t *GetMac() {return mac_.ether_addr_octet;};
    uint32_t GetIpAddress() {return ip_;};
    uint32_t GetInterfaceId() const {return intf_id_;}
    const string &GetName() const {return ifname_;};
    void FillObjectLog(sandesh_op::type op, KSyncIntfInfo &info);
    bool HasServiceVlan() const {return has_service_vlan_;};
    int GetNetworkId() const {return network_id_;};

private:
    friend class IntfKSyncObject;
    int Encode(sandesh_op::type op, char *buf, int buf_len);
    string ifname_;     // Key
    Interface::Type type_;
    uint32_t intf_id_;
    uint32_t vrf_id_;
    uint32_t fd_;       // FD opened for this
    bool has_service_vlan_;
    struct ether_addr mac_;
    uint32_t ip_;
    bool policy_enabled_;
    string analyzer_name_;
    Interface::MirrorDirection mirror_direction_;
    bool active_;
    size_t os_index_;
    int network_id_;
    VirtualHostInterface::SubType sub_type_;
    DISALLOW_COPY_AND_ASSIGN(IntfKSyncEntry);
};

class IntfKSyncObject : public KSyncDBObject {
public:
    static const int kInterfaceCount = 1000;        // Max interfaces

    IntfKSyncObject(DBTableBase *table) : 
        KSyncDBObject(table, kInterfaceCount), vnsw_if_mac(), test_mode() {};
    virtual ~IntfKSyncObject() {};

    virtual KSyncEntry *Alloc(const KSyncEntry *entry, uint32_t index) {
        const IntfKSyncEntry *intf = static_cast<const IntfKSyncEntry *>(entry);
        IntfKSyncEntry *ksync = new IntfKSyncEntry(intf, index);
        return static_cast<KSyncEntry *>(ksync);
    };

    virtual KSyncEntry *DBToKSyncEntry(const DBEntry *e) {
        const Interface *intf = static_cast<const Interface *>(e);
        IntfKSyncEntry *key = NULL;

        switch (intf->GetType()) {
        case Interface::ETH:
        case Interface::VMPORT:
        case Interface::HOST:
        case Interface::VHOST:
            key = new IntfKSyncEntry(intf);
            break;

        default:
            assert(0);
            break;
        }
        return static_cast<KSyncEntry *>(key);
    }

    static void Init(InterfaceTable *table);
    static void InitTest(InterfaceTable *table);
    static void Shutdown() {
        delete singleton_;
        singleton_ = NULL;
    }
 
    static IntfKSyncObject *GetKSyncObject() {return singleton_;};
    const char *PhysicalIntfMac() const {return vnsw_if_mac;};
    bool GetTestMode() const {return test_mode;};
private:
    static IntfKSyncObject *singleton_;
    char vnsw_if_mac[ETHER_ADDR_LEN];
    int test_mode;
    DISALLOW_COPY_AND_ASSIGN(IntfKSyncObject);
};

// Store kernel interface snapshot
class InterfaceKSnap {
public:
    typedef std::map<std::string, uint32_t> InterfaceKSnapMap;
    typedef std::map<std::string, uint32_t>::iterator InterfaceKSnapIter;
    typedef std::pair<std::string, uint32_t> InterfaceKSnapPair;

    static void Init();
    static void Shutdown();
    static InterfaceKSnap *GetInstance() { return singleton_; }

    virtual ~InterfaceKSnap();
    void KernelInterfaceData(vr_interface_req *r);
    bool FindInterfaceKSnapData(std::string &name, uint32_t &ip);
    bool Reset();

private:
    InterfaceKSnap();

    Timer *timer_;
    tbb::mutex mutex_;
    InterfaceKSnapMap data_map_;
    static const uint32_t timeout_ = 180000; // 3 minutes
    static InterfaceKSnap *singleton_;

    DISALLOW_COPY_AND_ASSIGN(InterfaceKSnap);
};

#endif // vnsw_agent_intf_ksync_h
