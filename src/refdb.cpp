// Copyright (c) 2017 The Merit Foundation developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "refdb.h"

#include "base58.h"
#include <limits>

#include <boost/multiprecision/float128.hpp> 

namespace referral
{
namespace
{
const char DB_CHILDREN = 'c';
const char DB_REFERRALS = 'r';
const char DB_REFERRALS_BY_KEY_ID = 'k';
const char DB_PARENT_KEY = 'p';
const char DB_ANV = 'a';
const char DB_LOT_SIZE = 's';
const char DB_LOT_VAL = 'v';

const size_t MAX_LEVELS = std::numeric_limits<size_t>::max();
const double LOG_MAX_UINT64 = std::log(std::numeric_limits<uint64_t>::max());
const size_t MAX_RESERVOIR_SIZE = 1000;
}

using ANVTuple = std::tuple<char, Address, CAmount>;

ReferralsViewDB::ReferralsViewDB(size_t nCacheSize, bool fMemory, bool fWipe, const std::string& db_name) :
    m_db(GetDataDir() / db_name, nCacheSize, fMemory, fWipe, true) {}

MaybeReferral ReferralsViewDB::GetReferral(const uint256& code_hash) const {
     MutableReferral referral;
     return m_db.Read(std::make_pair(DB_REFERRALS,code_hash), referral) ?
         MaybeReferral{referral} : MaybeReferral{};
}

MaybeAddress ReferralsViewDB::GetReferrer(const Address& address) const
{
    Address parent;
    return m_db.Read(std::make_pair(DB_PARENT_KEY, address), parent) ?
        MaybeAddress{parent} : MaybeAddress{};
}

ChildAddresses ReferralsViewDB::GetChildren(const Address& address) const
{
    ChildAddresses children;
    m_db.Read(std::make_pair(DB_CHILDREN, address), children);
    return children;
}

bool ReferralsViewDB::InsertReferral(const Referral& referral) {
    //write referral by code hash
    if(!m_db.Write(std::make_pair(DB_REFERRALS, referral.codeHash), referral))
        return false;

    // Typically because the referral should be written in order we should
    // be able to find the parent referral. We can then write the child->parent
    // mapping of public addresses
    Address parent_address;
    if(auto parent_referral = GetReferral(referral.previousReferral))
        parent_address = parent_referral->pubKeyId;

    if(!m_db.Write(std::make_pair(DB_PARENT_KEY, referral.pubKeyId), parent_address))
        return false;

    // Now we update the children of the parent address by inserting into the
    // child address array for the parent.
    ChildAddresses children;
    m_db.Read(std::make_pair(DB_CHILDREN, parent_address), children);

    children.push_back(referral.pubKeyId);
    if(!m_db.Write(std::make_pair(DB_CHILDREN, parent_address), children))
        return false;

    return true;
}

bool ReferralsViewDB::RemoveReferral(const Referral& referral) {
    if(!m_db.Erase(std::make_pair(DB_REFERRALS, referral.codeHash)))
        return false;

    Address parent_address;
    if(auto parent_referral = GetReferral(referral.previousReferral))
        parent_address = parent_referral->pubKeyId;

    if(!m_db.Erase(std::make_pair(DB_PARENT_KEY, referral.pubKeyId)))
        return false;

    ChildAddresses children;
    m_db.Read(std::make_pair(DB_CHILDREN, parent_address), children);

    children.erase(std::remove(std::begin(children), std::end(children), referral.pubKeyId), std::end(children));
    if(!m_db.Write(std::make_pair(DB_CHILDREN, parent_address), children))
        return false;

    return true;
}

bool ReferralsViewDB::ReferralCodeExists(const uint256& code_hash) const {
    return m_db.Exists(std::make_pair(DB_REFERRALS, code_hash));
}

bool ReferralsViewDB::WalletIdExists(const Address& address) const
{
    return m_db.Exists(std::make_pair(DB_PARENT_KEY, address));
}

/**
 * Updates ANV for the address and all parents. Note change can be negative if
 * there was a debit.
 */

bool ReferralsViewDB::UpdateANV(char addressType, const Address& start_address, CAmount change)
{
    debug("\tUpdateANV: %s + %d", CMeritAddress(addressType, start_address).ToString(), change);
    MaybeAddress address = start_address;
    size_t levels = 0;

    //MAX_LEVELS guards against cycles in DB
    while(address && levels < MAX_LEVELS)
    {
        //it's possible address didn't exist yet so an ANV of 0 is assumed.
        ANVTuple anv;
        m_db.Read(std::make_pair(DB_ANV, *address), anv);

        if(levels == 0) {
            std::get<0>(anv) = addressType;
            std::get<1>(anv) = start_address;
        }

        debug(
                "\t\t %d %s %d + %d",
                levels,
                CMeritAddress(std::get<0>(anv),std::get<1>(anv)).ToString(),
                std::get<2>(anv),
                change);

        std::get<2>(anv) += change;

        assert(std::get<2>(anv) >= 0);

        if(!m_db.Write(std::make_pair(DB_ANV, *address), anv)) {
            //TODO: Do we rollback anv computation for already processed address?
            // likely if we can't write then rollback will fail too.
            // figure out how to mark database as corrupt.
            return false;
        }

        address = GetReferrer(*address);
        levels++;
    }

    // We should never have cycles in the DB.
    // Hacked? Bug?
    assert(levels < MAX_LEVELS && "reached max levels. Referral DB cycle detected");
    return true;
}

MaybeAddressANV ReferralsViewDB::GetANV(const Address& address) const
{
    ANVTuple anv;
    return m_db.Read(std::make_pair(DB_ANV, address), anv) ? 
        MaybeAddressANV{{ std::get<0>(anv), std::get<1>(anv), std::get<2>(anv) }} : 
        MaybeAddressANV{};
}

AddressANVs ReferralsViewDB::GetAllANVs() const
{
    std::unique_ptr<CDBIterator> iter{m_db.NewIterator()};
    iter->SeekToFirst();

    AddressANVs anvs;
    auto address = std::make_pair(DB_ANV, Address{});
    while(iter->Valid())
    {
        //filter non ANV addresss
        if(!iter->GetKey(address)) {
            iter->Next();
            continue;
        }

        if(address.first != DB_ANV) {
            iter->Next();
            continue;
        }

        ANVTuple anv;
        if(!iter->GetValue(anv)) {
            iter->Next();
            continue;
        }

        anvs.push_back({
                std::get<0>(anv),
                std::get<1>(anv),
                std::get<2>(anv)
                });

        iter->Next();
    }
    return anvs;
}

AddressANVs ReferralsViewDB::GetAllRewardableANVs() const
{
    std::unique_ptr<CDBIterator> iter{m_db.NewIterator()};
    iter->SeekToFirst();

    AddressANVs anvs;
    auto address = std::make_pair(DB_ANV, Address{});
    while(iter->Valid())
    {
        //filter non ANV addresss
        if(!iter->GetKey(address)) {
            iter->Next();
            continue;
        }

        if(address.first != DB_ANV) {
            iter->Next();
            continue;
        }

        ANVTuple anv;
        if(!iter->GetValue(anv)) {
            iter->Next();
            continue;
        }

        const auto addressType = std::get<0>(anv);
        if(addressType != 1 && addressType != 2) {
            iter->Next();
            continue;
        }

        anvs.push_back({
                addressType,
                std::get<1>(anv),
                std::get<2>(anv)
                });

        iter->Next();
    }
    return anvs;
}


/**
 * This function uses a modified version of the weighted random sampling algorithm
 * by Efraimidis and Spirakis 
 * (https://www.sciencedirect.com/science/article/pii/S002001900500298X).
 *
 * Instead of computing R=rand^(1/W) where rand is some uniform random value
 * between [0,1] and W is the ANV, we will compute log(R). 
 */
bool ReferralsViewDB::AddAddressToLottery(const uint256& rand_value, const Address& address)
{
    auto maybe_anv = GetANV(address);
    if(!maybe_anv) return false;

    const auto rand_uint64 = rand_value.GetUint64(0);

    /* 
     * We need to compute the weighted key of the address which is originally
     * computed in RES by rand^(1/W). where rand is a uniform random value between
     * [0,1] and W is a weight. The weight in our case is the ANV of the address.
     *
     * Instead of computing the power above we will take the log as the weighted
     * key instead. log(rand^(1/W)) = log(rand) / W.
     *
     * We can think of rand_uint64 as a random value between 0-1.0 if we take 
     * rand_uint64 and divide it the max uint64_t. 
     *
     * rand = rand_uint64/max_uint64_t
     *
     * log(rand) = log(rand_uint64/max_uint64_t) 
     *           = log(rand_uint64) - log(max_uint64_t)
     */ 
    const boost::multiprecision::float128 rand = 
        std::log(rand_uint64) - LOG_MAX_UINT64;

    //We should get a negative number here.
    assert(rand <= 0);

    const boost::multiprecision::float128 anv_f = maybe_anv->anv;

    const auto weighted_key = rand / anv_f;

    auto heap_size = GetLotteryHeapSize();

    //TODO Implement a min heap on top of the DB. 
    //Store X amount of values. 
    //  IF heap.size < X THEN
    //      heap.insert weighted_key, address
    //  ELSE IF heap.min < weighted_key THEN
    //      heap.pop_min
    //      heap.insert weighted_key, address
    //  ELSE      

    return true;
}

std::size_t ReferralsViewDB::GetLotteryHeapSize() const
{
    std::size_t size = 0;
    m_db.Read(DB_LOT_SIZE, size);
    return size;
}

using LotteryHeapValue = std::pair<WeightedKey, Address>;

MaybeWeightedKey ReferralsViewDB::GetLotteryMinKey() const
{
    LotteryHeapValue v;
    return m_db.Read(std::make_pair(DB_LOT_VAL, 0), v) ? 
        MaybeWeightedKey{v.first} : 
        MaybeWeightedKey{};
}

bool ReferralsViewDB::InsertLotteryAddress(
        const WeightedKey& key,
        const Address& address)
{

    auto pos = GetLotteryHeapSize();

    if(pos >= MAX_RESERVOIR_SIZE) return false;

    if(!m_db.Write(DB_LOT_SIZE, pos + 1))
        return false;

    while(pos != 0)
    {
        auto parent_pos = pos % 2 == 0 ? pos >> 2 : (pos - 1) >> 2;

        LotteryHeapValue parent_value;
        if(!m_db.Read(std::make_pair(DB_LOT_VAL, parent_pos), parent_value)) {
            return false;
        }

        //We found out spot
        if(key > parent_value.first) {
            break;
        }

        //Push our parent down since we are moving up.
        if(!m_db.Write(std::make_pair(DB_LOT_VAL, pos), parent_value)) {
            return false;
        }

        pos = parent_pos;
    }

    //write final value
    if(!m_db.Write(std::make_pair(DB_LOT_VAL, pos), std::make_pair(key, address))) {
        return false;
    }

    return true;
}
}
