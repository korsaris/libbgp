/**
 * @file bgp-rib6.cc
 * @author Nato Morichika <nat@nat.moe>
 * @brief The IPv6 BGP Routing Information Base.
 * @version 0.1
 * @date 2019-07-21
 * 
 * @copyright Copyright (c) 2019
 * 
 */
#include <string.h>
#include <arpa/inet.h>
#include "bgp-rib6.h"

namespace libbgp {

/**
 * @brief Construct a new BgpRib6Entry.
 * 
 * @param r Route of this entry.
 * @param src Originating BGP speaker's ID in network bytes order.
 * @param nexthop_global Global IPv6 address of nexthop.
 * @param nexthop_linklocal Link local IPv6 address of nexthop. (if none, use NULL)
 * @param attribs Path attributes for this entry.
 */
BgpRib6Entry::BgpRib6Entry (Prefix6 r, uint32_t src, const uint8_t nexthop_global[16], 
    const uint8_t nexthop_linklocal[16], const std::vector<std::shared_ptr<BgpPathAttrib>> attribs)
    : route(r) {

    memcpy(this->nexthop_global, nexthop_global, 16);
    if (nexthop_linklocal != NULL) memcpy(this->nexthop_linklocal, nexthop_linklocal, 16);
    else memset(this->nexthop_linklocal, 0, 16);
    src_router_id = src;
    this->attribs = attribs;
}

/**
 * @brief Construct a new BgpRib6 object with logging.
 * 
 * @param logger Log handler to use.
 */
BgpRib6::BgpRib6(BgpLogHandler *logger) {
    this->logger = logger;
    update_id = 0;
}

bool BgpRib6::insertPriv(uint32_t src_router_id, const Prefix6 &route, 
        const uint8_t nexthop_global[16], const uint8_t nexthop_linklocal[16], 
        const std::vector<std::shared_ptr<BgpPathAttrib>> &attribs, int32_t weight) {
    BgpRib6Entry new_entry(route, src_router_id, nexthop_global, nexthop_linklocal, attribs);
    new_entry.update_id = update_id;
    new_entry.weight = weight;

    for (std::vector<BgpRib6Entry>::const_iterator entry = rib.begin(); entry != rib.end(); entry++) {
        if (entry->route == route && entry->src_router_id == src_router_id) {
            if (new_entry > *entry) {
                rib.erase(entry);
                
                rib.push_back(new_entry);

                LIBBGP_LOG(logger, INFO) {
                    uint8_t prefix_arr[16];
                    route.getPrefix(prefix_arr);
                    char src_router_id_str[INET_ADDRSTRLEN], prefix_str[INET6_ADDRSTRLEN];
                    inet_ntop(AF_INET, &src_router_id, src_router_id_str, INET_ADDRSTRLEN);
                    inet_ntop(AF_INET6, prefix_arr, prefix_str, INET6_ADDRSTRLEN);
                    logger->log(INFO, "BgpRib6::insert: (updated) group %d, scope %s, route %s/%d\n", new_entry.update_id, src_router_id_str, prefix_str, route.getLength());
                }

                return true;
            } else return false;
        }
    }
    
    LIBBGP_LOG(logger, INFO) {
        uint8_t prefix_arr[16];
        route.getPrefix(prefix_arr);
        char src_router_id_str[INET_ADDRSTRLEN], prefix_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_router_id, src_router_id_str, INET_ADDRSTRLEN);
        inet_ntop(AF_INET6, prefix_arr, prefix_str, INET6_ADDRSTRLEN);
        logger->log(INFO, "BgpRib6::insert: (new_entry) group %d, scope %s, route %s/%d\n", new_entry.update_id, src_router_id_str, prefix_str, route.getLength());
    }

    mutex.lock();
    rib.push_back(new_entry);
    mutex.unlock();
    return true;
}

/**
 * @brief Insert a local route into RIB.
 * 
 * Local routes are routes inserted to the RIB by user. The scope (src_router_id)
 * of local routes are 0. This method will create necessary path attribues
 * before inserting entry to RIB (AS_PATH, ORIGIN, NEXT_HOP). 
 * 
 * The logger pointer passed in is for attribues. (so if a attribute failed to 
 * deserialize, it will print to the provided logger).
 * 
 * To remove an entry inserted with this method, use 0 as `src_router_id`.
 * 
 * @param logger Pointer to logger for the created path attributes to use. 
 * @param route Route.
 * @param nexthop_global Global IPv6 address of nexthop.
 * @param nexthop_linklocal Link local IPv6 address of nexthop. (if none, use NULL)
 * @param weight weight of this entry.
 * @return const BgpRib6Entry* Inserted route entry.
 * @retval NULL failed to insert.
 * @retval !=NULL Inserted route entry.
 */
const BgpRib6Entry* BgpRib6::insert(BgpLogHandler *logger, const Prefix6 &route,
    const uint8_t nexthop_global[16], const uint8_t nexthop_linklocal[16], int32_t weight) {
    std::vector<std::shared_ptr<BgpPathAttrib>> attribs;
    BgpPathAttribOrigin *origin = new BgpPathAttribOrigin(logger);
    BgpPathAttribAsPath *as_path = new BgpPathAttribAsPath(logger, true);
    origin->origin = IGP;

    attribs.push_back(std::shared_ptr<BgpPathAttrib>(origin));
    attribs.push_back(std::shared_ptr<BgpPathAttrib>(as_path));

    BgpRib6Entry new_entry(route, 0, nexthop_global, nexthop_linklocal, attribs);
    new_entry.weight = weight;
    uint64_t use_update_id = update_id;

    for (const BgpRib6Entry &entry : rib) {
        if (entry.src_router_id == 0 && entry.route == route) {
            logger->log(ERROR, "BgpRib6::insert: route exists.");
            return NULL;
        }

        // see if we can group this entry to other local entries
        if (entry.src_router_id == 0) {
            if (memcmp(new_entry.nexthop_global, entry.nexthop_global, 16) == 0 &&
                memcmp(new_entry.nexthop_linklocal, entry.nexthop_linklocal, 16) == 0) {
                use_update_id = entry.update_id;
            }
        }
    }

    std::lock_guard<std::recursive_mutex> lock(mutex);
    new_entry.update_id = use_update_id;
    if (use_update_id == update_id) update_id++;
    rib.push_back(new_entry);

    return &(rib.back());
}

/**
 * @brief Insert a local route into RIB.
 * 
 * Same as the other local insert, but this one notify the rev_bus after
 * inserting route.
 * 
 * @param logger Pointer to logger for the created path attributes to use. 
 * @param route Route.
 * @param nexthop_global Global IPv6 address of nexthop.
 * @param nexthop_linklocal Link local IPv6 address of nexthop. (if none, use NULL)
 * @param rev_bus The route event bus to publish to add event.
 * @param weight weight of this entry.
 * @return const BgpRib6Entry* Inserted route entry.
 * @retval NULL failed to insert.
 * @retval !=NULL Inserted route entry.
 */
const BgpRib6Entry* BgpRib6::insert(BgpLogHandler *logger, 
        const Prefix6 &route, const uint8_t nexthop_global[16], 
        const uint8_t nexthop_linklocal[16], RouteEventBus *rev_bus, int32_t weight) {

    const BgpRib6Entry *entry = insert(logger, route, nexthop_global, nexthop_linklocal, weight);

    if (entry != NULL) {
        Route6AddEvent add_event;
        add_event.routes.push_back(entry->route);
        add_event.attribs = entry->attribs;
        rev_bus->publish(NULL, add_event);
    }

    return entry;
}

/**
 * @brief Insert local routes into RIB.
 * 
 * Same as the other local insert, but this one insert mutiple routes.
 * 
 * @param logger Pointer to logger for the created path attributes to use. 
 * @param routes Routes.
 * @param nexthop_global Global IPv6 address of nexthop.
 * @param nexthop_linklocal Link local IPv6 address of nexthop. (if none, use NULL)
 * @param weight weight of this entry.
 * @return const std::vector<BgpRib6Entry*> Insert routes.
 */
const std::vector<BgpRib6Entry> BgpRib6::insert(BgpLogHandler *logger, 
        const std::vector<Prefix6> &routes, const uint8_t nexthop_global[16], 
        const uint8_t nexthop_linklocal[16], int32_t weight) {
    std::vector<BgpRib6Entry> inserted;
    std::vector<std::shared_ptr<BgpPathAttrib>> attribs;
    BgpPathAttribOrigin *origin = new BgpPathAttribOrigin(logger);
    BgpPathAttribAsPath *as_path = new BgpPathAttribAsPath(logger, true);
    origin->origin = IGP;

    attribs.push_back(std::shared_ptr<BgpPathAttrib>(origin));
    attribs.push_back(std::shared_ptr<BgpPathAttrib>(as_path));

    for (const Prefix6 &route : routes) {
        bool entry_exist = false;

        for (const BgpRib6Entry &entry : rib) {
            if (entry.src_router_id != 0) continue;
            if (entry.route == route) {
                entry_exist = true;
                break;
            }
        }

        if (entry_exist) continue;

        BgpRib6Entry new_entry (route, 0, nexthop_global, nexthop_linklocal, attribs);
        new_entry.update_id = update_id;
        new_entry.weight = weight;
        rib.push_back(new_entry);
        inserted.push_back(rib.back());
    }

    update_id++;
    return inserted;
}

/**
 * @brief Insert local routes into RIB.
 * 
 * Same as the other local insert, but this one insert mutiple routes and notify
 * the event bus.
 * 
 * @param logger Pointer to logger for the created path attributes to use. 
 * @param routes Routes.
 * @param nexthop_global Global IPv6 address of nexthop.
 * @param nexthop_linklocal Link local IPv6 address of nexthop. (if none, use NULL)
 * @param rev_bus The event bus to use
 * @param weight weight of this entry.
 * @return const std::vector<BgpRib6Entry*> Insert routes.
 */
const std::vector<BgpRib6Entry> BgpRib6::insert(BgpLogHandler *logger, 
    const std::vector<Prefix6> &routes, const uint8_t nexthop_global[16], 
    const uint8_t nexthop_linklocal[16], RouteEventBus *rev_bus, int32_t weight) {
        
    const std::vector<BgpRib6Entry> inserted =
        insert(logger, routes, nexthop_global, nexthop_linklocal, weight);
    
    if (inserted.size() > 0) {
        Route6AddEvent add_event;
        add_event.attribs = (inserted.begin())->attribs;

        for (const BgpRib6Entry &entry : inserted) {
            add_event.routes.push_back(entry.route);
        }

        rev_bus->publish(NULL, add_event);
    }

    return inserted;
}

/**
 * @brief Insert new entries into RIB.
 * 
 * @param src_router_id Originating BGP speaker's ID in network bytes order.
 * @param route Route to be inserted.
 * @param nexthop_global Global IPv6 address of nexthop.
 * @param nexthop_linklocal Link local IPv6 address of nexthop. (if none, use NULL)
 * @param attrib Path attribute.
 * @param weight weight of this entry.
 * @return true Route inserted/replaced.
 * @return false Route already exist and the existing one has lower metric.
 */
bool BgpRib6::insert(uint32_t src_router_id, const Prefix6 &route, 
    const uint8_t nexthop_global[16], const uint8_t nexthop_linklocal[16], 
    const std::vector<std::shared_ptr<BgpPathAttrib>> &attribs, int32_t weight) {

    bool inserted = insertPriv(src_router_id, route, nexthop_global, nexthop_linklocal, attribs, weight);
    if (inserted) update_id++;

    return inserted;
}

/**
 * @brief Insert new entries into RIB.
 * 
 * @param src_router_id Originating BGP speaker's ID in network bytes order.
 * @param routes List of routes.
 * @param nexthop_global Global IPv6 address of nexthop.
 * @param nexthop_linklocal Link local IPv6 address of nexthop. (if none, use NULL)
 * @param attrib Path attribute. 
 * @param weight weight of this entry.
 * @param weight weight of this entry.
 * @return ssize_t Number of routes inserted.
 * @retval -1 Failed to insert routes.
 * @retval >=0 Number of routes inserted.
 */
ssize_t BgpRib6::insert(uint32_t src_router_id, const std::vector<Prefix6> &routes, 
    const uint8_t nexthop_global[16], const uint8_t nexthop_linklocal[16], 
    const std::vector<std::shared_ptr<BgpPathAttrib>> &attrib, int32_t weight) {
    size_t inserted = 0;
    for (const Prefix6 &r : routes) {
        if (insertPriv(src_router_id, r, nexthop_linklocal, nexthop_global, attrib, weight)) inserted++;
    }
    update_id++;
    return inserted;
}

/**
 * @brief Withdraw a route from RIB.
 * 
 * @param src_router_id Originating BGP speaker's ID in network bytes order.
 * @param route Route.
 * @return true Route dropped.
 * @return false Route not dropped. Likely becuase such route does not exist.
 */
bool BgpRib6::withdraw(uint32_t src_router_id, const Prefix6 &route) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (std::vector<BgpRib6Entry>::const_iterator entry = rib.begin(); entry != rib.end(); entry++) {
        if (entry->route == route && entry->src_router_id == src_router_id) {
            LIBBGP_LOG(logger, INFO) {
                uint8_t prefix_arr[16];
                route.getPrefix(prefix_arr);
                char src_router_id_str[INET_ADDRSTRLEN], prefix_str[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET, &src_router_id, src_router_id_str, INET_ADDRSTRLEN);
                inet_ntop(AF_INET6, prefix_arr, prefix_str, INET6_ADDRSTRLEN);
                logger->log(INFO, "BgpRib6::withdraw: (dropped) scope %s, route %s/%d\n", src_router_id_str, prefix_str, route.getLength());
            }
            rib.erase(entry);
            return true;
        }
    }

    return false;
}

/**
 * @brief Withdraw routes from RIB.
 * 
 * @param src_router_id Originating BGP speaker's ID in network bytes order.
 * @param routes Routes.
 * @return ssize_t Number of routes dropped.
 * @retval -1 Failed to drop routes.
 * @retval >=0 Number of routes dropped.
 */
ssize_t BgpRib6::withdraw(uint32_t src_router_id, const std::vector<Prefix6> &routes) {
    size_t dropped = 0;
    for (const Prefix6 &r : routes) {
        if (withdraw(src_router_id, r)) dropped++;
    }
    return dropped;
}

/**
 * @brief Drop all routes from RIB that originated from a BGP speaker.
 * 
 * @param src_router_id src_router_id Originating BGP speaker's ID in network bytes order.
 * @return std::vector<Route> Dropped routes.
 */
std::vector<Prefix6> BgpRib6::discard(uint32_t src_router_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<Prefix6> dropped_routes;
    for (std::vector<BgpRib6Entry>::const_iterator entry = rib.begin(); entry != rib.end();) {
        if (entry->src_router_id == src_router_id) {
            dropped_routes.push_back(entry->route);
            LIBBGP_LOG(logger, INFO) {
                    uint8_t prefix_arr[16];
                    entry->route.getPrefix(prefix_arr);
                    char src_router_id_str[INET_ADDRSTRLEN], prefix_str[INET6_ADDRSTRLEN];
                    inet_ntop(AF_INET, &src_router_id, src_router_id_str, INET_ADDRSTRLEN);
                    inet_ntop(AF_INET6, prefix_arr, prefix_str, INET6_ADDRSTRLEN);
                logger->log(INFO, "BgpRib6::discard: (dropped) scope %s, route %s/%d\n", src_router_id_str, prefix_str, entry->route.getLength());
            }
            rib.erase(entry);
        } else entry++;
    }

    return dropped_routes;
}

/**
 * @brief Lookup a destination in RIB.
 * 
 * @param dest The destination address.
 * @return const BgpRib6::BgpRib6Entry* Matching entry.
 * @retval NULL no match found.
 * @retval BgpRib6Entry* Matching entry.
 */
const BgpRib6Entry* BgpRib6::lookup(const uint8_t dest[16]) const {
        const BgpRib6Entry *selected_entry = NULL;

    for (const BgpRib6Entry &entry : rib) {
        const Prefix6 &route = entry.route;
        if (route.includes(dest)) 
            selected_entry = selectEntry(&entry, selected_entry);
    }

    return selected_entry;
}

/**
 * @brief Scoped RIB lookup.
 * 
 * Simular to lookup with only one argument but only lookup in routes from the
 * given BGP speaker.
 * 
 * @param src_router_id Originating BGP speaker's ID in network bytes order.
 * @param dest The destination address.
 * @return const BgpRib6::BgpRib6Entry* Matching entry.
 * @retval NULL no match found.
 * @retval BgpRib6Entry* Matching entry.
 */
const BgpRib6Entry* BgpRib6::lookup(uint32_t src_router_id, const uint8_t dest[16]) const {
    const BgpRib6Entry *selected_entry = NULL;

    for (const BgpRib6Entry &entry : rib) {
        if (entry.src_router_id != src_router_id) continue;
        const Prefix6 &route = entry.route;
        if (route.includes(dest)) 
            selected_entry = selectEntry(&entry, selected_entry);
    }

    return selected_entry;
}

/**
 * @brief Get the RIB.
 * 
 * @return const std::vector<BgpRib6Entry>& The RIB.
 */
const std::vector<BgpRib6Entry>& BgpRib6::get() const {
    return rib;
}

}