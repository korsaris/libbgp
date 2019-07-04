#include "bgp-update-message.h"
#include "bgp-error.h"
#include "bgp-errcode.h"
#include "value-op.h"
#include <arpa/inet.h>
#include <assert.h>

namespace bgpfsm {

BgpUpdateMessage::BgpUpdateMessage(bool use_4b_asn) {
    this->use_4b_asn = use_4b_asn;
}

BgpPathAttrib& BgpUpdateMessage::getAttrib(uint8_t type) {
    for (std::shared_ptr<BgpPathAttrib> attrib : path_attribute) {
        if (attrib->type_code == type) return *attrib;
    }

    throw "no such attribute";
}

const BgpPathAttrib& BgpUpdateMessage::getAttrib(uint8_t type) const {
    for (const std::shared_ptr<BgpPathAttrib> &attrib : path_attribute) {
        if (attrib->type_code == type) return *attrib;
    }

    throw "no such attribute";
}

bool BgpUpdateMessage::hasAttrib(uint8_t type) const {
    for (const std::shared_ptr<BgpPathAttrib> &attrib : path_attribute) {
        if (attrib->type_code == type) return true;
    }

    return false;
}

bool BgpUpdateMessage::addAttrib(const BgpPathAttrib &attrib) {
    if (hasAttrib(attrib.type_code)) return false;

    path_attribute.push_back(std::shared_ptr<BgpPathAttrib>(attrib.clone()));
    return true;
}

bool BgpUpdateMessage::setAttribs(const std::vector<std::shared_ptr<BgpPathAttrib>> &attrs) {
    path_attribute.clear();
    for (const std::shared_ptr<BgpPathAttrib> &attrib : attrs) {
        path_attribute.push_back(std::shared_ptr<BgpPathAttrib>(attrib->clone()));
    }
    return true;
}

bool BgpUpdateMessage::dropAttrib(uint8_t type) {
    for (auto attr = path_attribute.begin(); attr != path_attribute.end(); attr++) {
        if ((*attr)->type_code == type) {
            path_attribute.erase(attr);
            return true;
        }
    }

    return false;
}

bool BgpUpdateMessage::dropNonTransitive() {
    bool removed = false;

    for (auto attr = path_attribute.begin(); attr != path_attribute.end();) {
        if (!(*attr)->transitive) {
            removed = true;
            path_attribute.erase(attr);
        } else attr++;
    }

    return removed;
}

bool BgpUpdateMessage::updateAttribute(const BgpPathAttrib &attrib) {
    dropAttrib(attrib.type_code);
    return addAttrib(attrib);
}

bool BgpUpdateMessage::setNextHop(uint32_t nexthop) {
    BgpPathAttribNexthop nh = BgpPathAttribNexthop();
    nh.next_hop = nexthop;
    return updateAttribute(nh);
}

bool BgpUpdateMessage::prepend(uint32_t asn) {
    if (use_4b_asn) {
        // in 4b mode, prepend 4b asn to AS_PATH directly.

        // AS4_PATH can't exist in 4b mode
        if (hasAttrib(AS4_PATH)) {
            _bgp_error("BgpUpdateMessage::prepend: we have AS4_PATH attribute but we are running in 4b mode. " 
                       "consider restoreAsPath().\n");
            return false;
        }

        if (!hasAttrib(AS_PATH)) {
            BgpPathAttribAsPath *path = new BgpPathAttribAsPath(use_4b_asn);
            path->prepend(asn);
            path_attribute.push_back(std::shared_ptr<BgpPathAttrib>(path));
            return true;
        }

        BgpPathAttribAsPath &path = dynamic_cast<BgpPathAttribAsPath &>(getAttrib(AS_PATH));
        if (!path.is_4b) {
            _bgp_error("BgpUpdateMessage::prepend: existing AS_PATH is 2b but we are running in 4b mode. " 
                       "consider restoreAsPath().\n");
            return false;
        }

        return path.prepend(asn);
    } else {
        // in 2b-mode, prepend 2b asn to AS_PATH and update AS4_PATH.
        // (yes, you don't update AS4_PATH as a 2b-speaker, but simplicity we do that for now)
        // FIXME: don't change as4_path if both side disabled 4b support

        uint16_t prep_asn = asn >= 0xffff ? 23456 : asn;

        if (!hasAttrib(AS_PATH)) {
            BgpPathAttribAsPath *path = new BgpPathAttribAsPath(use_4b_asn);
            path->prepend(prep_asn);
            path_attribute.push_back(std::shared_ptr<BgpPathAttrib>(path));
        } else {
            BgpPathAttribAsPath &path = dynamic_cast<BgpPathAttribAsPath &>(getAttrib(AS_PATH));
            if (path.is_4b) {
                _bgp_error("BgpUpdateMessage::prepend: existing AS_PATH is 4b but we are running in 2b mode. " 
                           "consider downgradeAsPath().\n");
                return false;
            }
            if(!path.prepend(prep_asn)) return false;
        }

        if (hasAttrib(AS4_PATH)) {
            BgpPathAttribAs4Path &path4 = dynamic_cast<BgpPathAttribAs4Path &>(getAttrib(AS4_PATH));
            if(!path4.prepend(prep_asn)) return false;
        }

        return true;
    }
}


bool BgpUpdateMessage::restoreAsPath() {
    if (!hasAttrib(AS_PATH)) return true;

    BgpPathAttribAsPath &path = dynamic_cast<BgpPathAttribAsPath &>(getAttrib(AS_PATH));
    if (path.is_4b) return true;

    // no AS4_PATH, just make AS_PATH 4b
    if (!hasAttrib(AS4_PATH)) {
        std::vector<BgpAsPathSegment> new_segs;

        for (const BgpAsPathSegment &seg2 : path.as_paths) {
            if (seg2.is_4b) {
                _bgp_error("BgpUpdateMessage::restoreAsPath: 4b seg found in 2b attrib.\n");
                return false;
            }

            BgpAsPathSegment new_seg (true, seg2.type);
            for (uint16_t asn : seg2.value) {
                if (asn == 23456) {
                    _bgp_error("BgpUpdateMessage::restoreAsPath: warning: AS_TRANS found but no AS4_PATH.\n");
                }
                new_seg.value.push_back(asn);
            }

            new_segs.push_back(new_seg);
        }

        path.as_paths = new_segs;
        path.is_4b = true;
        return true;
    }

    // we have AS4_PATH recorver AS_TRANS.
    std::vector<uint32_t> full_as_path;
    const BgpPathAttribAs4Path &as4_path = dynamic_cast<const BgpPathAttribAs4Path &>(getAttrib(AS4_PATH));
    for (const BgpAsPathSegment &seg4 : as4_path.as4_paths) {
        if (!seg4.is_4b) {
            _bgp_error("BgpUpdateMessage::restoreAsPath: bad as4_path: found 2b seg.\n");
            return false;
        }
        
        if (seg4.type == AS_SEQUENCE) {
            const std::vector<uint32_t> &part = seg4.value;
            full_as_path.insert(full_as_path.end(), part.begin(), part.end());
        }
    }

    // AS4_PATH should be removed.
    dropAttrib(AS4_PATH);

    bool has_4b = false;

    // find the iterator to first asn > 0xffff
    std::vector<uint32_t>::const_iterator iter_4b = full_as_path.begin();

    for(; iter_4b != full_as_path.end(); iter_4b++) {
        has_4b = true;
        if (*iter_4b > 0xffff) break;
    }

    std::vector<BgpAsPathSegment> new_segs;

    for (const BgpAsPathSegment &seg2 : path.as_paths) {
        std::vector<uint32_t>::const_iterator local_iter = iter_4b;
        if (seg2.is_4b) {
            _bgp_error("BgpUpdateMessage::restoreAsPath: 4b seg found in 2b attrib.\n");
            return false;
        }

        BgpAsPathSegment new_seg (true, seg2.type);

        // increment the local_iter iterator?
        bool incr_iter = false;
        for (uint16_t asn : seg2.value) {
            uint32_t new_asn = asn;

            // AS4_PATH avaliale & not ended?
            if (has_4b && local_iter != full_as_path.end()) {

                // we found AS_TRANS, we need to replace it with last asn in AS_TRANS
                if (new_asn == 23456) {

                    // we have hit our first AS_TRANS. from now on, we need to move the AS4_PATH
                    // iterator too.
                    incr_iter = true;
                    new_asn = *local_iter;
                } else if (new_asn != *local_iter) {
                    _bgp_error("BgpUpdateMessage::restoreAsPath: warning: AS_PATH and AS4_PATH does not match.\n");
                }

                if (incr_iter) local_iter++;
            }
            
            new_seg.value.push_back(new_asn);
        }

        new_segs.push_back(new_seg);
    }

    path.is_4b = true;
    path.as_paths = new_segs;
    return true;
    
}

bool BgpUpdateMessage::downgradeAsPath() {
    if (!hasAttrib(AS_PATH)) return true;

    BgpPathAttribAsPath &path = dynamic_cast<BgpPathAttribAsPath &>(getAttrib(AS_PATH));
    if (!path.is_4b) return true;

    std::vector<BgpAsPathSegment> new_segs;
    BgpPathAttribAs4Path path4;

    for (const BgpAsPathSegment &seg4 : path.as_paths) {
        if (!seg4.is_4b) {
            _bgp_error("BgpUpdateMessage::downgradeAsPath: 2b seg found in 4b attrib.\n");
            return false;
        }

        BgpAsPathSegment new_seg (false, seg4.type);
        for (uint32_t asn : seg4.value) {
            uint16_t new_as = asn >= 0xffff ? 23456 : asn;
            new_seg.value.push_back(new_as);
        }

        path4.as4_paths.push_back(seg4);
        new_segs.push_back(new_seg);
    }
    
    updateAttribute(path4);
    path.is_4b = false;
    path.as_paths = new_segs;
    return true;
}

bool BgpUpdateMessage::restoreAggregator() {
    if (!hasAttrib(AGGREATOR)) return true;

    BgpPathAttribAggregator &aggr = dynamic_cast<BgpPathAttribAggregator &>(getAttrib(AGGREATOR));
    aggr.is_4b = true;

    if (!hasAttrib(AS4_AGGREGATOR)) return true;
    
    const BgpPathAttribAs4Aggregator &aggr4 = dynamic_cast<const BgpPathAttribAs4Aggregator &>(getAttrib(AS4_AGGREGATOR));
    aggr.aggregator = aggr4.aggregator;
    aggr.aggregator_asn = aggr4.aggregator_asn4;

    return true;
}

bool BgpUpdateMessage::downgradeAggregator() {
    if (!hasAttrib(AGGREATOR)) return true;
    
    BgpPathAttribAggregator &aggr = dynamic_cast<BgpPathAttribAggregator &>(getAttrib(AGGREATOR));
    aggr.is_4b = false;

    BgpPathAttribAs4Aggregator aggr4;
    aggr4.aggregator = aggr.aggregator;
    aggr4.aggregator_asn4 = aggr.aggregator_asn;
    updateAttribute(aggr4);

    if (aggr.aggregator_asn >= 0xffff) aggr.aggregator_asn = 23456;

    return true;
}

bool BgpUpdateMessage::setWithdrawn(const std::vector<Route> &routes) {
    withdrawn_routes = routes;
    return true;
}

bool BgpUpdateMessage::addWithdrawn(uint32_t prefix, uint8_t length) {
    Route route (prefix, length);
    withdrawn_routes.push_back(route);
    return true;
}

bool BgpUpdateMessage::addWithdrawn(const Route &route) {
    withdrawn_routes.push_back(route);
    return true;
}

bool BgpUpdateMessage::setNlri(const std::vector<Route> &routes) {
    nlri = routes;
    return true;
}

bool BgpUpdateMessage::addNlri(uint32_t prefix, uint8_t length) {
    Route route(prefix, length);
    nlri.push_back(route);
    return true;
}

bool BgpUpdateMessage::addNlri(const Route &route) {
    nlri.push_back(route);
    return true;
}

bool BgpUpdateMessage::validateAttribs() {
    bool has_origin = false;
    bool has_nexthop = false;
    bool has_as_path = false;

    uint32_t typecode_bitsmap = 0;

    for (std::vector<std::shared_ptr<BgpPathAttrib>>::const_iterator attr_iter = path_attribute.begin(); 
        attr_iter != path_attribute.end(); attr_iter++) {
        uint8_t type_code = (*attr_iter)->type_code;

        if (type_code == AS_PATH) has_as_path = true;
        else if (type_code == NEXT_HOP) has_nexthop = true;
        else if (type_code == ORIGIN) has_origin = true;

        if ((typecode_bitsmap >> type_code) & 1U) {
            _bgp_error("BgpUpdateMessage::validateAttribs:: duplicated attribute type in list: %d\n", type_code);
            setError(E_UPDATE, E_ATTR_LIST, NULL, 0);
            return false;
        }

        typecode_bitsmap &= ~(1UL << type_code);
    }

    if (!(has_as_path && has_nexthop && has_origin)) {
        _bgp_error("BgpUpdateMessage::validateAttribs: mandatory attribute(s) missing.\n");
        setError(E_UPDATE, E_MISS_WELL_KNOWN, NULL, 0);
        return false;
    }

    return true;
}

ssize_t BgpUpdateMessage::parse(const uint8_t *from, size_t msg_sz) {
    if (msg_sz < 4) {
        uint8_t _err_data = msg_sz;
        setError(E_HEADER, E_LENGTH, &_err_data, sizeof(uint8_t));
        _bgp_error("BgpUpdateMessage::parse: invalid open message size: %d.\n", msg_sz);
        return -1;
    }

    const uint8_t *buffer = from;

    uint16_t withdrawn_len = ntohs(getValue<uint16_t>(&buffer)); // len: 2

    if (withdrawn_len > msg_sz - 4) { // -4: two length fields (withdrawn len + attrib len)
        _bgp_error("BgpUpdateMessage::parse: withdrawn routes length overflows message.\n");
        setError(E_UPDATE, E_UNSPEC, NULL, 0);
        return -1;
    }

    uint16_t parsed_withdrawn_len = 0;
    
    while (parsed_withdrawn_len < withdrawn_len) {
        if (withdrawn_len - parsed_withdrawn_len < 1) {
            _bgp_error("BgpUpdateMessage::parse: unexpected end of withdrawn routes list.\n");
            setError(E_UPDATE, E_UNSPEC, NULL, 0);
            return -1;
        }

        uint8_t route_len = getValue<uint8_t>(&buffer); // len2: 1
        parsed_withdrawn_len++;
        if (route_len > 32) {
            _bgp_error("BgpUpdateMessage::parse: invalid route len in withdrawn routes: %d\n", route_len);
            setError(E_UPDATE, E_UNSPEC, NULL, 0);
            return -1;
        }

        size_t route_buffer_len = (route_len + 7) / 8;
        if (parsed_withdrawn_len + route_buffer_len > withdrawn_len) {
            _bgp_error("BgpUpdateMessage::parse: withdrawn route overflows routes list.\n");
            setError(E_UPDATE, E_UNSPEC, NULL, 0);
            return -1;
        }

        uint32_t prefix = 0;
        memcpy(&prefix, buffer, route_buffer_len);
        Route route(prefix, route_len);
        withdrawn_routes.push_back(route);

        buffer += route_buffer_len; // len2: route_buffer_len
        parsed_withdrawn_len += route_buffer_len;
    }

    assert(parsed_withdrawn_len == withdrawn_len);

    uint16_t attribute_len = ntohs(getValue<uint16_t>(&buffer)); // len: 2
    if ((size_t) (attribute_len + withdrawn_len + 4) > msg_sz) {
        _bgp_error("BgpUpdateMessage::parse: attribute list length overflows message buffer.\n");
        setError(E_UPDATE, E_ATTR_LIST, NULL, 0);
        return -1;
    }

    uint16_t parsed_attribute_len = 0;

    while (parsed_attribute_len < attribute_len) {
        if (attribute_len - parsed_attribute_len < 3) {
            _bgp_error("BgpUpdateMessage::parse: unexpected end of attribute list.\n");
            setError(E_UPDATE, E_UNSPEC, NULL, 0);
            return -1;
        }

        int8_t attr_type = BgpPathAttrib::GetTypeFromBuffer(buffer, attribute_len - parsed_attribute_len);

        if (attr_type < 0) {
            _bgp_error("BgpUpdateMessage::parse: failed to parse attribute type.\n");
            setError(E_UPDATE, E_UNSPEC, NULL, 0);
            return -1;
        }

        BgpPathAttrib *attrib = NULL;

        switch(attr_type) {
            case ORIGIN: attrib = new BgpPathAttribOrigin(); break;
            case AS_PATH: attrib = new BgpPathAttribAsPath(use_4b_asn); break;
            case NEXT_HOP: attrib = new BgpPathAttribNexthop(); break;
            case MULTI_EXIT_DISC: attrib = new BgpPathAttribMed(); break;
            case LOCAL_PREF: attrib = new BgpPathAttribLocalPref(); break;
            case ATOMIC_AGGREGATE: attrib =  new BgpPathAttribAtomicAggregate(); break;
            case AGGREATOR: attrib = new BgpPathAttribAggregator(use_4b_asn); break;
            case COMMUNITY: attrib = new BgpPathAttribCommunity(); break;
            case AS4_PATH: attrib = new BgpPathAttribAs4Path(); break;
            case AS4_AGGREGATOR: attrib = new BgpPathAttribAs4Aggregator(); break;
            default: attrib = new BgpPathAttrib(); break;
        }

        assert(attrib != NULL);

        ssize_t attrib_parsed = attrib->parse(buffer, attribute_len - parsed_attribute_len);

        if (attrib_parsed < 0) {
            forwardParseError(*attrib);
            delete attrib;
            return -1;
        }

        buffer += attrib_parsed;
        parsed_attribute_len += attrib_parsed;
        path_attribute.push_back(std::shared_ptr<BgpPathAttrib>(attrib));
    }

    if (!validateAttribs()) return -1;

    assert(parsed_attribute_len == attribute_len);

    // 4: len fields (withdrawn len & attrib len)
    size_t nlri_len = msg_sz - 4 - parsed_attribute_len - parsed_withdrawn_len;
    size_t parsed_nlri_len = 0;

    while (parsed_nlri_len < nlri_len) {
        if (nlri_len - parsed_nlri_len < 1) {
            _bgp_error("BgpOpenMessage::parse: unexpected end of nlri.\n");
            setError(E_UPDATE, E_UNSPEC, NULL, 0);
            return -1;
        }

        uint8_t route_len = getValue<uint8_t>(&buffer); // len2: 1
        parsed_nlri_len++;
        if (route_len > 32) {
            _bgp_error("BgpUpdateMessage::parse: invalid route len in nlri routes: %d\n", route_len);
            setError(E_UPDATE, E_UNSPEC, NULL, 0);
            return -1;
        }

        size_t route_buffer_len = (route_len + 7) / 8;
        if (parsed_nlri_len + route_buffer_len > nlri_len) {
            _bgp_error("BgpUpdateMessage::parse: nlri route overflows routes list.\n");
            setError(E_UPDATE, E_UNSPEC, NULL, 0);
            return -1;
        }

        uint32_t prefix = 0;
        memcpy(&prefix, buffer, route_buffer_len);
        Route route(prefix, route_len);
        nlri.push_back(route);

        buffer += route_buffer_len; // len2: route_buffer_len
        parsed_nlri_len += route_buffer_len;
    }

    assert(parsed_nlri_len + parsed_attribute_len + parsed_withdrawn_len + 4 == msg_sz);

    return msg_sz;
}

ssize_t BgpUpdateMessage::write(uint8_t *to, size_t buf_sz) const {
    if (buf_sz < 4) {
        _bgp_error("BgpUpdateMessage::write: destination buffer too small.\n");
        return -1;
    }

    size_t tot_written = 0;
    uint8_t *buffer = to;

    // keep a pointer to len field to write length to later
    uint8_t *withdrawn_routes_len_ptr = buffer; 
    buffer++; // skip the length field for now

    size_t written_withdrawn_length = 0;

    for (const Route &route : withdrawn_routes) {
        uint8_t route_len = route.getLength();
        size_t pfx_buf_sz = (route_len + 7) / 8;

        // 1: this prefix len field
        if (1 + written_withdrawn_length + pfx_buf_sz + tot_written > buf_sz) {
            _bgp_error("BgpUpdateMessage::write: destination buffer too small.\n");
            return -1;
        }

        putValue<uint8_t>(&buffer, route_len);
        memcpy(buffer, &route_len, pfx_buf_sz);

        buffer += pfx_buf_sz;
        written_withdrawn_length += 1 + pfx_buf_sz;
    }

    // now, put the length
    putValue<uint16_t>(&withdrawn_routes_len_ptr, htons(written_withdrawn_length));

    tot_written += written_withdrawn_length + 2; // + 2: the length field

    // keep a pointer to len field to write length to later
    uint8_t *route_attrib_len_ptr = buffer;
    buffer++;

    size_t written_attrib_length = 0;

    for (const std::shared_ptr<BgpPathAttrib> &attr : path_attribute) {
        ssize_t buf_left = buf_sz - written_attrib_length - tot_written;
        assert(buf_left >= 0);

        ssize_t write_ret = attr->write(buffer, buf_left);

        if (write_ret < 0) return -1;
        written_attrib_length += write_ret;
    }

    // put the length
    putValue<uint16_t>(&route_attrib_len_ptr, htons(written_attrib_length));

    tot_written += written_attrib_length + 2;

    size_t written_nlri_len = 0;

    for (const Route &route : nlri) {
        uint8_t route_len = route.getLength();

        size_t pfx_buf_sz = (route_len + 7) / 8;

        // 1: this prefix len field
        if (1 + written_nlri_len + pfx_buf_sz + tot_written > buf_sz) {
            _bgp_error("BgpUpdateMessage::write: destination buffer too small.\n");
            return -1;
        }

        putValue<uint8_t>(&buffer, route_len);
        memcpy(buffer, &route_len, pfx_buf_sz);

        buffer += pfx_buf_sz;
        written_nlri_len += 1 + pfx_buf_sz;
    }

    tot_written += written_nlri_len;

    return tot_written;
}

ssize_t BgpUpdateMessage::doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const {
    size_t written = 0;
    written += _print(indent, to, buf_sz, "UpdateMessage {\n");
    indent++; {
        if (withdrawn_routes.size() == 0) written += _print(indent, to, buf_sz, "WithdrawnRoutes { }\n");
        else {
            written += _print(indent, to, buf_sz, "WithdrawnRoutes {\n");
            indent++; {
                for (const Route &route : withdrawn_routes) {
                    uint32_t prefix = route.getPrefix();
                    written += _print(indent, to, buf_sz, "Route { %s/%d }\n", inet_ntoa(*(struct in_addr*) &prefix), route.getLength());
                }
            }; indent--;
            written += _print(indent, to, buf_sz, "}\n");
        }

        if (path_attribute.size() == 0) written += _print(indent, to, buf_sz, "PathAttributes { }\n");
        else {
            written += _print(indent, to, buf_sz, "PathAttributes {\n");
            indent++; {
                for (const std::shared_ptr<BgpPathAttrib> &attr : path_attribute) {
                    written += attr->doPrint(indent, to, buf_sz);
                }
            }; indent--;
            written += _print(indent, to, buf_sz, "}\n");
        }

        if (nlri.size() == 0) written += _print(indent, to, buf_sz, "NLRI { }\n");
        else {
            written += _print(indent, to, buf_sz, "NLRI {\n");
            indent++; {
                for (const Route &route : nlri) {
                    uint32_t prefix = route.getPrefix();
                    written += _print(indent, to, buf_sz, "Route { %s/%d }\n", inet_ntoa(*(struct in_addr*) &prefix), route.getLength());
                }
            }; indent--;
            written += _print(indent, to, buf_sz, "}\n");
        }
    }; indent--;
    written += _print(indent, to, buf_sz, "}\n");
    return written;
}

}
