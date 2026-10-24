// Binding surface extracted from src/javascript/src/binding/global_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=registry, LocalProto]
interface GlobalBindingRegistry {
  [CFunction=CustomElementsDefine] void define(any a0, any a1);
  [CFunction=CustomElementsGet] void get(any a0);
  [CFunction=CustomElementsWhenDefined] void whenDefined(any a0);
  [CFunction=CustomElementsUpgrade] void upgrade(any a0);
};

[Proto=list, LocalProto]
interface GlobalBindingList {
  [CFunction=MatchMediaAddListener] void addEventListener(any a0, any a1);
  [CFunction=MatchMediaRemoveListener] void removeEventListener(any a0, any a1);
  [CFunction=MatchMediaAddListener] void addListener(any a0);
  [CFunction=MatchMediaRemoveListener] void removeListener(any a0);
};
