#include "vu2.h"
#include "vu_priv.h"

unsigned long vuRBIteratorResyncs = 0, vuRBIteratorLost = 0;

VuRBIterator::VuRBIterator(VuRedBlackTree *coll)
    : VuIterator(coll), curr_(0), currKey_(0), currEnt_(NULL), stamp_(0)
{
    if (coll == NULL)
    {
        return;
    }

    curr_ = coll->map_.size();
    stamp_ = coll->stamp_;
}

VuRBIterator::~VuRBIterator()
{
}

VuEntity *VuRBIterator::At()
{
    VuRedBlackTree *rbt = static_cast<VuRedBlackTree *>(collection_);
    stamp_ = rbt->stamp_;

    if (curr_ >= rbt->map_.size())
    {
        curr_ = rbt->map_.size();
        currEnt_ = NULL;
        return NULL;
    }

    currKey_ = rbt->map_[curr_].key;
    currEnt_ = rbt->map_[curr_].ent;
    return currEnt_;
}

bool VuRBIterator::Resync()
{
    VuRedBlackTree *rbt = static_cast<VuRedBlackTree *>(collection_);
    const VuRedBlackTree::RBMap &m = rbt->map_;
    ++vuRBIteratorResyncs;
    stamp_ = rbt->stamp_;

    if (currEnt_ == NULL)
    {
        // was at end, a map iterator stays there
        curr_ = m.size();
        return true;
    }

    VuRedBlackTree::RBMap::size_type i = rbt->LowerBound(currKey_);

    for (; (i < m.size()) and (m[i].key == currKey_); ++i)
    {
        if (m[i].ent == currEnt_)
        {
            curr_ = i;
            return true;
        }
    }

    // our entry was erased: the old map iterator would have dangled. Stand on whatever
    // now follows it, and tell GetNext not to step past that.
    ++vuRBIteratorLost;
    curr_ = i;
    return false;
}

VuEntity *VuRBIterator::GetFirst()
{
    if (not collection_)
    {
        return NULL;
    }

    curr_ = 0;
    VuEntity *ret = At();

    if (ret == NULL)
    {
        return NULL;
    }

    if (ret->VuState() not_eq VU_MEM_ACTIVE)
    {
        return GetNext();
    }
    else
    {
        return ret;
    }
}

VuEntity *VuRBIterator::GetFirst(VU_KEY low)
{
    if (not collection_)
    {
        return NULL;
    }

    VuRedBlackTree *rbt = static_cast<VuRedBlackTree *>(collection_);
    curr_ = rbt->LowerBound(low);
    VuEntity *ret = At();

    if (ret == NULL)
    {
        return NULL;
    }

    if (ret->VuState() not_eq VU_MEM_ACTIVE)
    {
        return GetNext();
    }
    else
    {
        return ret;
    }
}

VuEntity *VuRBIterator::GetNext()
{
    if (not collection_)
    {
        return NULL;
    }

    VuRedBlackTree *rbt = static_cast<VuRedBlackTree *>(collection_);

    for (;;)
    {
        if ((stamp_ not_eq rbt->stamp_) and not Resync())
        {
            // already on the next entry
        }
        else if (curr_ < rbt->map_.size())
        {
            ++curr_;
        }
        else
        {
            return NULL;
        }

        VuEntity *ret = At();

        if (ret == NULL or ret->VuState() == VU_MEM_ACTIVE)
        {
            return ret;
        }
    }
}

VuEntity *VuRBIterator::CurrEnt()
{
    VuRedBlackTree *rbt = static_cast<VuRedBlackTree *>(collection_);

    if (stamp_ not_eq rbt->stamp_)
    {
        Resync();
    }

    return At();
}

VU_ERRCODE VuRBIterator::Cleanup()
{
    VuRedBlackTree *rbt = static_cast<VuRedBlackTree *>(collection_);
    curr_ = rbt->map_.size();
    currEnt_ = NULL;
    stamp_ = rbt->stamp_;
    return VU_SUCCESS;
}
