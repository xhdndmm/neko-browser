// Binding surface extracted from src/javascript/src/binding/intersection_observer_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=intersection_observer_proto]
interface IntersectionObserverBindingIntersectionObserverProto {
  [CFunction=IoObserverObserve] void observe(any a0);
  [CFunction=IoObserverUnobserve] void unobserve(any a0);
  [CFunction=IoObserverDisconnect] void disconnect();
  [CFunction=IoObserverTakeRecords] void takeRecords();
};
