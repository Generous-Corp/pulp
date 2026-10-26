---
breaking = true
summary = "pulp::osc::Receiver binds loopback by default; set ReceiverOptions::bind_address to 0.0.0.0 to accept LAN controllers"
---

## Breaking changes

- `pulp::osc::Receiver::listen` and `listen_with_options` bind `127.0.0.1`
  instead of every interface. A receiver that previously accepted packets from
  a hardware controller or another machine on the LAN now only hears the local
  machine until it opts in.
- `ReceiverOptions::bind_address` is new. It takes an IPv4 literal; hostnames
  and IPv6 literals make `listen` fail instead of being resolved.
- `Receiver::local_address()` is new and reports the address actually bound.

## Migration steps

- To keep LAN reachability, switch the call to `listen_with_options` and set
  `options.bind_address = std::string(pulp::osc::kBindAllInterfaces);`
  (`"0.0.0.0"`). Nothing else in the options changes.
- To restrict a receiver to one interface, pass that interface's IPv4 address
  as the literal.
- Code that only ever sent to `127.0.0.1` needs no change.
