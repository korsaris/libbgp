/**
 * @file bgp-path-attrib.h
 * @author Nato Morichika <nat@nat.moe>
 * @brief The BGP path attributes.
 * @version 0.1
 * @date 2019-07-04
 * 
 * @copyright Copyright (c) 2019
 * 
 */
#ifndef BGP_PATH_ATTR_H
#define BGP_PATH_ATTR_H

#include "serializable.h"
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <vector>

namespace libbgp {

/**
 * @brief BGP attribute type codes.
 * 
 */
enum BgpPathAttribType {
    UNKNOW = -1,
    RESERVED = 0,
    ORIGIN = 1,
    AS_PATH = 2,
    NEXT_HOP = 3,
    MULTI_EXIT_DISC = 4,
    LOCAL_PREF = 5,
    ATOMIC_AGGREGATE = 6,
    AGGREATOR = 7,
    COMMUNITY = 8,
    AS4_PATH = 17,
    AS4_AGGREGATOR = 18
    // TODO: RFC4760
};

/**
 * @brief The BgpPathAttrib class.
 * 
 */
class BgpPathAttrib : public Serializable {
public:

    /**
     * @brief Attribute flag: Optional.
     * 
     */
    bool optional;

    /**
     * @brief Attribute flag: Transitive
     * 
     */
    bool transitive;

    /**
     * @brief Attribute flag: partial
     * 
     */
    bool partial;

    /**
     * @brief Attribute flag: extended
     * 
     */
    bool extended;

    /**
     * @brief Attribute type code.
     * 
     */
    uint8_t type_code;

    BgpPathAttrib();
    BgpPathAttrib(const uint8_t *value, uint16_t val_len);

    // get attribute type from buffer, return -1 if failed.
    static int8_t GetTypeFromBuffer(const uint8_t *buffer, size_t buffer_sz);

    // print the attribute
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;

    // parse attribute 
    ssize_t parse(const uint8_t *buffer, size_t length);

    // write attribute
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;

    /**
     * @brief Clone the attribute.
     * 
     * @return BgpPathAttrib* Pointer to the cloned attribute.
     */
    virtual BgpPathAttrib* clone() const;

    virtual ~BgpPathAttrib();

protected:
    // utility function to parse header (flags, typecode, length) from buffer
    ssize_t parseHeader(const uint8_t *buffer, size_t length);

    // utility function to print flags
    ssize_t printFlags(size_t indent, uint8_t **to, size_t *buf_sz) const;

    // utility function to write header (flags, type_code) to buffer. notice
    // that length is not write to buffer by this function.
    ssize_t writeHeader(uint8_t *buffer, size_t buffer_sz) const;

    /**
     * @brief Attribute length. 
     * Length field is only used in deserialization for parseHeader() to pass
     * length field in header to the underlying deserializers. The length field
     * is ignored when serialize. (except BgpPathAttrib base, which acts as the
     * container for unknow attribute)
     */
    uint16_t value_len;

private:
    uint8_t* value_ptr;
};

/**
 * @brief BGP origin path attribute values
 * 
 */
enum BgpPathAttribOrigins {
    IGP = 0,
    EGP = 1,
    INCOMPLETE = 2
};

/**
 * @brief Origin Attribute
 * 
 */
class BgpPathAttribOrigin : public BgpPathAttrib {
public:
    BgpPathAttribOrigin();
    uint8_t origin;
    
    BgpPathAttrib* clone() const;
    ssize_t parse(const uint8_t *buffer, size_t length);
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;
};

/**
 * @brief `AS_PATH` segment types.
 * 
 */
enum BgpAsPathSegmentType {
    AS_SET = 1,
    AS_SEQUENCE = 2
};

/**
 * @brief An AS_PATH or AS4_PATH segment
 * 
 */
class BgpAsPathSegment {
public:
    BgpAsPathSegment(bool is_4b, uint8_t type);

    /**
     * @brief Is ASNs in this segment four octets?
     * 
     */
    bool is_4b;

    /**
     * @brief Segment type.
     * 
     */
    uint8_t type;
    size_t getCount() const;
    bool prepend(uint32_t asn);

    /**
     * @brief The segment value.
     * 
     */
    std::vector<uint32_t> value;
};

/**
 * @brief AS Path attribute.
 * 
 */
class BgpPathAttribAsPath : public BgpPathAttrib {
public:
    // is_4b: 4b ASN in AS_PATH?
    BgpPathAttribAsPath(bool is_4b);

    BgpPathAttrib* clone() const;

    /**
     * @brief The AS Path segments.
     * 
     */
    std::vector<BgpAsPathSegment> as_paths;

    /**
     * @brief Is ASNs in the attribute four octets?
     * 
     */
    bool is_4b;

    // prepend: utility function to prepend an ASN to path
    bool prepend(uint32_t asn);

    ssize_t parse(const uint8_t *buffer, size_t length);
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;
private:
    // addSeg: add a new AS_SEQUENCE with one ASN to AS_PATH 
    void addSeg(uint32_t asn);
};

/**
 * @brief Nexthop attribute.
 * 
 */
class BgpPathAttribNexthop : public BgpPathAttrib {
public:
    BgpPathAttribNexthop();

    /**
     * @brief The nexthop in network byte order.
     * 
     */
    uint32_t next_hop;

    BgpPathAttrib* clone() const;
    ssize_t parse(const uint8_t *buffer, size_t length);
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;
};

/**
 * @brief Multi Exit Discriminator attribute
 * 
 */
class BgpPathAttribMed : public BgpPathAttrib {
public:
    BgpPathAttribMed();

    /**
     * @brief MED.
     * 
     */
    uint32_t med;

    BgpPathAttrib* clone() const;
    ssize_t parse(const uint8_t *buffer, size_t length);
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;
};

/**
 * @brief Local Pref attribute.
 * 
 */
class BgpPathAttribLocalPref : public BgpPathAttrib {
public:
    BgpPathAttribLocalPref();

    /**
     * @brief Local Pref.
     * 
     */
    uint32_t local_pref;

    BgpPathAttrib* clone() const;
    ssize_t parse(const uint8_t *buffer, size_t length);
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;
};

/**
 * @brief Atomic aggregate attribute.
 * 
 */
class BgpPathAttribAtomicAggregate : public BgpPathAttrib {
public:
    BgpPathAttribAtomicAggregate();

    BgpPathAttrib* clone() const;
    ssize_t parse(const uint8_t *buffer, size_t length);
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;
};

/**
 * @brief Aggregator attribute.
 * 
 */
class BgpPathAttribAggregator : public BgpPathAttrib {
public:
    // is_4b: 4b asn in aggregator?
    BgpPathAttribAggregator(bool is_4b);

    /**
     * @brief Aggregator in network byte order.
     * 
     */
    uint32_t aggregator;

    /**
     * @brief Aggregator ASN.
     * 
     */
    uint32_t aggregator_asn;

    /**
     * @brief Is ASNs in the attribute four octets?
     * 
     */
    bool is_4b;

    BgpPathAttrib* clone() const;
    ssize_t parse(const uint8_t *buffer, size_t length);
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;
};

/**
 * @brief AS4_PATH attribute.
 * 
 */
class BgpPathAttribAs4Path : public BgpPathAttrib {
public:
    BgpPathAttribAs4Path();

    /**
     * @brief The AS4_PATH segments.
     * 
     */
    std::vector<BgpAsPathSegment> as4_paths;

    bool prepend(uint32_t asn);

    BgpPathAttrib* clone() const;
    ssize_t parse(const uint8_t *buffer, size_t length);
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;
private:
    void addSeg(uint32_t asn);
};

/**
 * @brief AS4_AGGREGATOR attribute.
 * 
 */
class BgpPathAttribAs4Aggregator : public BgpPathAttrib {
public:
    BgpPathAttribAs4Aggregator();

    /**
     * @brief Aggregator in network byte order.
     * 
     */
    uint32_t aggregator;

    /**
     * @brief Aggregator ASN.
     * 
     */
    uint32_t aggregator_asn4;

    BgpPathAttrib* clone() const;
    ssize_t parse(const uint8_t *buffer, size_t length);
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;
};

/**
 * @brief BGP community attribute.
 * 
 */
class BgpPathAttribCommunity : public BgpPathAttrib {
public:
    BgpPathAttribCommunity();

    /**
     * @brief Raw community attribute in network byte order.
     * 
     */
    uint32_t community;

    BgpPathAttrib* clone() const;
    ssize_t parse(const uint8_t *buffer, size_t length);
    ssize_t write(uint8_t *buffer, size_t buffer_sz) const;
    ssize_t doPrint(size_t indent, uint8_t **to, size_t *buf_sz) const;
};

}
#endif // BGP_PATH_ATTR_H