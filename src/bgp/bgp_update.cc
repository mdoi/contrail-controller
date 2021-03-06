/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_update.h"

#include <time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#include "base/logging.h"
#include "bgp/bgp_route.h"
#include "bgp/bgp_table.h"

RouteUpdate::RouteUpdate(BgpRoute *route, int queue_id)
    : UpdateEntry(UpdateEntry::UPDATE),
    route_(route),
    queue_id_(queue_id),
    flags_(0) {
}

RouteUpdate::~RouteUpdate() {
    ClearUpdateInfo();
    ClearHistory();
}

//
// Set the given UpdateInfoSList in the RouteUpdate to the given value.
//
// Note that the back pointers from the individual UpdateInfo elements
// to the RouteUpdate will be set up when the RouteUpdate gets enqueued
// to an UpdateQueue.
//
void RouteUpdate::SetUpdateInfo(UpdateInfoSList &uinfo_slist) {
    assert(updates_->empty());
    updates_.swap(uinfo_slist);
}

//
// Get rid of all the UpdateInfo elements in the RouteUpdate. Each element
// is also deleted.
//
void RouteUpdate::ClearUpdateInfo() {
    updates_->clear_and_dispose(UpdateInfoDisposer());
}

//
// Remove the UpdateInfo element from the UpdateInfoSList and delete the
// element.
//
// Return true if the UpdateInfoSList is now empty.
//
bool RouteUpdate::RemoveUpdateInfo(UpdateInfo *uinfo) {
    updates_->erase_and_dispose(updates_->iterator_to(*uinfo),
            UpdateInfoDisposer());
    return updates_->empty();
}

//
// Find the UpdateInfo element with matching RibOutAttr.
//
UpdateInfo *RouteUpdate::FindUpdateInfo(const RibOutAttr &roattr) {
    for (UpdateInfoSList::List::iterator iter = updates_->begin();
         iter != updates_->end(); iter++) {
        if (iter->roattr == roattr) return iter.operator->();
    }
    return NULL;
}

//
// Find the UpdateInfo element with matching RibOutAttr, full of const
// goodness so it can be used by CompareUpdateInfo.
//
const UpdateInfo *RouteUpdate::FindUpdateInfo(const RibOutAttr &roattr) const {
    for (UpdateInfoSList::List::const_iterator iter = updates_->begin();
         iter != updates_->end(); iter++) {
        if (iter->roattr == roattr) return iter.operator->();
    }
    return NULL;
}

//
// Reset the given RibPeerSet from the target of all UpdateInfo elements. Get
// rid of any elements whose target becomes empty.
//
void RouteUpdate::ResetUpdateInfo(RibPeerSet &peerset) {
    for (UpdateInfoSList::List::iterator iter = updates_->begin();
         iter != updates_->end(); ) {
        iter->target.Reset(peerset);
        if (iter->target.empty()) {
            iter = updates_->erase_and_dispose(iter, UpdateInfoDisposer());
        } else {
            iter++;
        }
    }
}

//
// Compare the list of UpdateInfo elements in the RouteUpdate with the
// given list. Uses brute force since the lists will typically contain
// just a single element.
//
// Return true if the lists are logically the same.
//
bool RouteUpdate::CompareUpdateInfo(const UpdateInfoSList &uinfo_slist) const {
    if (updates_->size() != uinfo_slist->size())
        return false;
    for (UpdateInfoSList::List::const_iterator iter = uinfo_slist->begin();
         iter != uinfo_slist->end(); ++iter) {
        const UpdateInfo *uinfo = FindUpdateInfo(iter->roattr);
        if (!uinfo || uinfo->target != iter->target)
            return false;
    }
    return true;
}

//
// Merge the list of UpdateInfo elements into the RouteUpdate. If we find
// an UpdateInfo with the same RibOutAttr, we modify it's target in place.
// Otherwise, we reset the target bits in any existing UpdateInfos in the
// RouteUpdate and move the UpdateInfo from the list into the RouteUpdate.
//
void RouteUpdate::MergeUpdateInfo(UpdateInfoSList &uinfo_slist) {
    for (UpdateInfoSList::List::iterator iter = uinfo_slist->begin();
         iter != uinfo_slist->end(); ) {
        UpdateInfo *uinfo = FindUpdateInfo(iter->roattr);
        if (uinfo) {
            uinfo->target |= iter->target;
            iter = uinfo_slist->erase_and_dispose(iter, UpdateInfoDisposer());
        } else {
            UpdateInfo &temp_uinfo = *iter;
            ResetUpdateInfo(temp_uinfo.target);
            iter = uinfo_slist->erase(iter);
            updates_->push_front(temp_uinfo);
        }
    }
}

//
// Set the given AdvertiseSList in the RouteUpdate to the given value.
//
// Should be used only for testing.
//
void RouteUpdate::SetHistory(AdvertiseSList &history) {
    assert(history_->empty());
    history_.swap(history);
}

//
// Get rid of all the AdvertiseInfo elements in the RouteUpdate. Each element
// is also deleted.
//
void RouteUpdate::ClearHistory() {
    history_->clear_and_dispose(AdvertiseInfoDisposer());
}

//
// Move history from RouteUpdate to RouteState.
//
void RouteUpdate::MoveHistory(RouteState *rstate) {
    AdvertiseSList adv_slist;
    SwapHistory(adv_slist);
    rstate->SwapHistory(adv_slist);
}

//
// Find the history element with matching RibOutAttr.
//
const AdvertiseInfo *RouteUpdate::FindHistory(const RibOutAttr &roattr) const {
    for (AdvertiseSList::List::const_iterator iter = history_->begin();
         iter != history_->end(); iter++) {
        if (iter->roattr == roattr) return iter.operator->();
    }
    return NULL;
}

//
// Update the history information for the RouteUpdate to include the given
// RibOutAttr and the RibPeerSet. This may entail an update to an existing
// AdvertiseInfo or the creation of a new one, as well as possible deletion
// of an existing one.
//
void RouteUpdate::UpdateHistory(RibOut *ribout, const RibOutAttr *roattr,
        const RibPeerSet &bits) {

    // The history information may reside in the RouteUpdate itself or in
    // the associated UpdateList if the RouteUpdate in on an UpdateList.
    AdvertiseSList &adv_slist =
        (OnUpdateList() ? GetUpdateList(ribout)->History() : history_);

    // Traipse through all the AdvertiseInfo elements in the history.  We
    // obviously need to find one with a matching RibOutAttr if it exists
    // and update it's RibPeerSet.   Additionally, we also want to reset
    // the RibPeerSet in all other elements since we may have previously
    // advertised different attributes to some of the peers in the set.
    AdvertiseInfo *ainfo = NULL;
    for (AdvertiseSList::List::iterator iter = adv_slist->begin();
         iter != adv_slist->end(); ) {

        // Remember if we find the matching element, and move on to the
        // next one.
        if (iter->roattr == *roattr) {
            assert(ainfo == NULL);
            ainfo = iter.operator->();
            iter++;
            continue;
        }

        // TBD: optimize to reuse an element with the same RibPeerSet but
        // different attribtues.

        // Reset the bits in the current element.  If the RibPeerSet in the
        // element becomes empty, get rid of it.
        iter->bitset.Reset(bits);
        if (iter->bitset.empty()) {
            AdvertiseSList::List::iterator prev_iter = iter++;
            adv_slist->erase_and_dispose(prev_iter, AdvertiseInfoDisposer());
        } else {
            ++iter;
        }
    }

    // Update an existing element if we found one or create a new one with
    // the appropriate state.
    if (roattr->IsReachable()) {
        if (ainfo == NULL) {
            ainfo = new AdvertiseInfo(roattr);
            adv_slist->push_front(*ainfo);
        }
        ainfo->bitset |= bits;
    } else {
        assert(ainfo == NULL);
    }
}

//
// Return true if this RouteUpdate has been advertised to anyone. We simply
// go through all the AdvertiseInfo elements in the history and check if we
// have at least one that's reachable.
//
bool RouteUpdate::IsAdvertised() const {
    for (AdvertiseSList::List::const_iterator iter = history_->begin();
         iter != history_->end(); ++iter) {
        if (iter->roattr.IsReachable()) {
            return true;
        }
    }
    return false;
}

//
// Upgrade this RouteUpdate to an UpdateList.  Creates a new UpdateList and
// adds the RouteUpdate to it.  Also moves the history from the RouteUpdate
// to the UpdateList.
//
// Returns the new UpdateList.
//
UpdateList *RouteUpdate::MakeUpdateList() {
    UpdateList *uplist = new UpdateList;
    AdvertiseSList history;
    SwapHistory(history);
    uplist->SwapHistory(history);
    uplist->AddUpdate(this);
    return uplist;
}

//
// Return the UpdateList that contains this RouteUpdate.  Assumes that the
// RouteUpdate is on an UpdateList.
//
UpdateList *RouteUpdate::GetUpdateList(RibOut *ribout) {
    assert(FlagIsSet(RouteUpdate::F_LIST));
    DBState *dbstate = route_->GetState(ribout->table(), ribout->listener_id());
    UpdateList *uplist = dynamic_cast<UpdateList *>(dbstate);
    assert(uplist);
    return uplist;
}

//
// Update the timestamp in this RouteUpdate.

// The code expects that the APIs use the underlying CPU performance counters
// in order to provide high resolution timestamps.
//
void RouteUpdate::set_tstamp_now() {
#ifdef __APPLE__
    tstamp_ = mach_absolute_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    tstamp_ = ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

//
// Set the given AdvertiseSList in the UpdateList to the given value.
//
// Should be used only for testing.
//
void UpdateList::SetHistory(AdvertiseSList &history) {
    assert(history_->empty());
    history_.swap(history);
}

//
// Move history from UpdateList to RouteState.
//
void UpdateList::MoveHistory(RouteState *rstate) {
    AdvertiseSList adv_slist;
    SwapHistory(adv_slist);
    rstate->SwapHistory(adv_slist);
}

//
// Move history from UpdateList to RouteUpdate.
//
void UpdateList::MoveHistory(RouteUpdate *rt_update) {
    AdvertiseSList adv_slist;
    SwapHistory(adv_slist);
    rt_update->SwapHistory(adv_slist);
}

//
// Find the history element with matching RibOutAttr.
//
const AdvertiseInfo *UpdateList::FindHistory(const RibOutAttr &roattr) const {
    for (AdvertiseSList::List::const_iterator iter = history_->begin();
         iter != history_->end(); iter++) {
        if (iter->roattr == roattr) return iter.operator->();
    }
    return NULL;
}

//
// Add a RouteUpdate to this UpdateList.  Sets up the association between
// the RouteUpdate and the UpdateList.
//
// Assumes that RouteUpdate has no history i.e. AdvertiseSList is empty.
//
void UpdateList::AddUpdate(RouteUpdate *rt_update) {
    list_.push_back(rt_update);
    rt_update->set_update_list();
}

//
// Remove a RouteUpdate from this UpdateList.  Takes care of removing the
// linkage between the UpdateList and the RouteUpdate.
//
void UpdateList::RemoveUpdate(RouteUpdate *rt_update) {
    rt_update->clear_update_list();
    list_.remove(rt_update);
}

//
// Find the RouteUpdate for the given queue.
//
RouteUpdate *UpdateList::FindUpdate(int queue_id) {
    for (List::iterator iter = list_.begin(); iter != list_.end(); ++iter) {
        if ((*iter)->queue_id() == queue_id)
            return *iter;
    }
    return NULL;
}

//
// Downgrade this UpdateList to a RouteUpdate if there's only 1 RouteUpdate
// on it. Takes care of removing the linkage between the UpdateList and the
// last RouteUpdate and moves history from the UpdateList to the RouteUpdate.
//
// Note that the caller is responsible for deleting the UpdateList since it
// holds a lock on the mutex in the UpdateList.
//
// Returns the last/only RouteUpdate if successful, NULL otherwise.
//
RouteUpdate *UpdateList::MakeRouteUpdate() {
    assert (list_.size() != 0);

    if (list_.size() > 1)
        return NULL;

    RouteUpdate *rt_update = list_.front();
    rt_update->clear_update_list();
    MoveHistory(rt_update);
    return rt_update;
}
