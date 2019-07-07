#include <libbgp/bgp-fsm.h>
#include <libbgp/route-event-bus.h>
#include <arpa/inet.h>
#include <sys/types.h>

// In this example, we don't connect to any remote peer. Instead, we have 
// another BGP FSM running as a "remote" BGP speaker. 

// We can create our own RouteEventReceiver to get notified when route changes.
// The routing information is available with BgpRib object, but you won't know 
// if there have been new routes added to the RIB.
// RouteEventReceiver is an interface for RouteEventBus participant. 
// RouteEventBus is usually used by BGP FSMs to communicate with each other. 
// (since every FSM handle a single BGP session, and there might be multiple BGP
// sessions running at a time) RouteEventBus allow BGP FSMs to pass route 
// add/withdrawn updates to other FSMs. (collision detection is also done 
// through RouteEventBus)
class MyEventHandler : public libbgp::RouteEventReceiver {
public:
    MyEventHandler(const char *name) {
        this->name = name;
    }

protected:
    bool handleRouteEvent(const libbgp::RouteEvent &ev) {
        if (ev.type == libbgp::ADD) {
            const libbgp::RouteAddEvent &add_ev = dynamic_cast<const libbgp::RouteAddEvent &>(ev);
            printRoutes("add", add_ev.routes);
        }
        if (ev.type == libbgp::WITHDRAW) {
            const libbgp::RouteWithdrawEvent &wd_ev = dynamic_cast<const libbgp::RouteWithdrawEvent &>(ev);
            printRoutes("withdraw", wd_ev.routes);
        }

        // we are just peeking the events, no need to report event handled
        return false;
    }

private:
    void printRoutes(const char *type, const std::vector<libbgp::Route> &routes) {
        char ip_str[INET_ADDRSTRLEN];
        for (const libbgp::Route &r : routes) {
            uint32_t prefix = r.getPrefix();
            inet_ntop(AF_INET, &prefix, ip_str, INET_ADDRSTRLEN);
            printf("%s: %s: %s/%d\n", name, type, ip_str, r.getLength());
        }
    }

    const char *name;
};

// Implement our output handler to pass data directly to another BGP FSM. 
// BgpOutHandler is used by BGP FSM to write BGP message to peer. 
// Usually, BGP FSM will write messages to a peer with a TCP socket. (a file 
// descriptor). In that case, we use FdOutHandler, which comes with libbgp. 
// However, here we want to talk to another BGP FSM running in the same program.
class PipedOutHandler : public libbgp::BgpOutHandler {
public:
    PipedOutHandler() {
        other = NULL;
    }

    void setPeer(libbgp::BgpFsm *other) {
        this->other = other;
    }

    bool handleOut(const uint8_t *buffer, size_t length) {
        return other->run(buffer, length) >= 0;
    };

private:
    libbgp::BgpFsm *other;
};

// Since we have two FSMs here, we want to label their log output so we know
// where the log is comming from.
class MyLoghandler : public libbgp::BgpLogHandler {
public:
    MyLoghandler(const char *name) {
        this->name = name;
    }

protected:
    void stdoutImpl(const char* str) {
        printf("%s stdout: %s", name, str);
    }

    void stderrImpl(const char* str) {
        printf("%s stderr: %s", name, str);
    }

private:
    const char *name;
};

int main(void) {
    /* create the "local" BGP speaker */
    libbgp::BgpConfig local_bgp_config;
    PipedOutHandler pipe_local; // create the output pipe
    MyLoghandler local_logger("local"); // create the logger for local speaker

    /* create the route event bus for our home-made receiver to print routes */
    libbgp::RouteEventBus local_bus; // create the event bus
    MyEventHandler local_handler("local"); // create our event subscriber
    local_bus.subscribe(&local_handler); // subscribe to it

    // create the rib. the logger is optional. setting a logger enable verbose
    // output on rib.
    libbgp::BgpRib local_rib(&local_logger); 

    /* set config parameters for local speaker */
    local_bgp_config.asn = 65000; // set local ASN
    local_bgp_config.peer_asn = 65001; // set peer ASN
    local_bgp_config.use_4b_asn = true; // enable RFC 6793
    local_bgp_config.hold_timer = 120; // hold timer
    local_bgp_config.out_handler = &pipe_local; // handle output with bridge
    local_bgp_config.no_collision_detection = true; // no need for that

    local_bgp_config.rib = NULL; 

    // use our local event bus.
    local_bgp_config.rev_bus = &local_bus; 

    local_bgp_config.clock = NULL; // use system clock.
    local_bgp_config.verbose = true; // print out all messages.
    local_bgp_config.log_handler = &local_logger; 

    inet_pton(AF_INET, "10.0.0.1", &local_bgp_config.router_id); // router id

    // nexthop selection and nexthop validation is done with peering_lan_*
    // configutaion. For simplicity, we are disabling those checks here. Router
    // server example has demonstrated how peer_lan_* were used. For detailed 
    // usage, refer to the document.

    // always use 10.0.0.1 as nexthop. 
    inet_pton(AF_INET, "10.0.0.1", &local_bgp_config.nexthop); 
    local_bgp_config.forced_default_nexthop = true; 

    // don't validate nexthop of routes received from peer.
    local_bgp_config.no_nexthop_check = true; 

    /* create the "remote" BGP speaker */
    libbgp::BgpConfig remote_bgp_config;
    PipedOutHandler pipe_remote; // create the output pipe
    MyLoghandler remote_logger("remote"); // create the logger for remote speaker

    /* create the route event bus for our home-made receiver to print routes */
    libbgp::RouteEventBus remote_bus; // create the event bus
    MyEventHandler remote_handler("remote"); // create our event subscriber
    remote_bus.subscribe(&remote_handler); // subscribe to it

    libbgp::BgpRib remote_rib(&remote_logger); // create the rib.

    /* set config parameters for remote speaker */
    remote_bgp_config.asn = 65001; // set local ASN
    remote_bgp_config.peer_asn = 65000; // set peer ASN
    remote_bgp_config.use_4b_asn = true; // enable RFC 6793
    remote_bgp_config.hold_timer = 120; // hold timer
    remote_bgp_config.out_handler = &pipe_remote; // handle output with bridge
    remote_bgp_config.no_collision_detection = true; // no need for that

    remote_bgp_config.rib = NULL; 

    // use our remote event bus.
    remote_bgp_config.rev_bus = &remote_bus; 

    remote_bgp_config.clock = NULL; // use system clock.
    remote_bgp_config.verbose = true; // print out all messages.
    remote_bgp_config.log_handler = &remote_logger; 

    inet_pton(AF_INET, "10.0.0.2", &remote_bgp_config.router_id); // router id

    // nexthop selection and nexthop validation is done with peering_lan_*
    // configutaion. For simplicity, we are disabling those checks here. Router
    // server example has demonstrated how peer_lan_* were used. For detailed 
    // usage, refer to the document.

    // always use 10.0.0.2 as nexthop. 
    inet_pton(AF_INET, "10.0.0.2", &remote_bgp_config.nexthop); 
    remote_bgp_config.forced_default_nexthop = true; 

    // don't validate nexthop of routes received from peer.
    remote_bgp_config.no_nexthop_check = true; 
    
    /* create the FSMs and connect them with each other */
    libbgp::BgpFsm local(local_bgp_config);
    libbgp::BgpFsm remote(remote_bgp_config);
    pipe_local.setPeer(&remote);
    pipe_remote.setPeer(&local);

    // sent OPEN message from local.
    local.start();

    // BGP peering has established from this point, let send some routes from
    // local to remote.

    // a route, 172.30.0.0/24.
    libbgp::Route r_172_30_24 ("172.30.0.0", 24);

    // put the route in RIB. 
    const libbgp::BgpRibEntry *inserted = local_rib.insert(&local_logger, r_172_30_24, local_bgp_config.nexthop);

    // BGP FSM will send all routes to peer (filtered with egress route filters 
    // if set) when peering established. However, if the session is already 
    // started, BGP FSM will have no way knowing there's new routes added to the
    // RIB. We will need to notify BGP FSM with route event bus. 

    // create an route-add event.
    libbgp::RouteAddEvent add_event;
    add_event.routes.push_back(inserted->route);
    add_event.attribs = inserted->attribs;

    // publish the event with event bus. The first parameter is pointer to the
    // publisher, and it is for ensuring publisher of the event does not
    // receive the event it published itself. You may use NULL if you are not
    // subscribed to the event bus.
    // When a BGP FSM receive an add route event from event bus, it will filter
    // the routes in event payload with egress route filters, and send routes
    // to the peer.
    local_bus.publish(&local_handler, add_event);

    // now let drop that route we just added from RIB.
    local_rib.withdraw(0, r_172_30_24);

    // and notify the FSM.
    libbgp::RouteWithdrawEvent withdraw_event;
    withdraw_event.routes.push_back(r_172_30_24);
    local_bus.publish(&local_handler, withdraw_event);

    // clean up
    local.stop();
    remote.stop();
    remote_bus.unsubscribe(&remote_handler);
    local_bus.unsubscribe(&local_handler);
    return 0;
}