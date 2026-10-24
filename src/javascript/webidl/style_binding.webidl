// Binding surface extracted from src/javascript/src/binding/style_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=style_proto]
interface StyleBindingStyleProto {
  [CFunction=StyleSetProperty] void setProperty(any a0, any a1);
  [CFunction=StyleGetPropertyValue] void getPropertyValue(any a0);
  [CFunction=StyleRemoveProperty] void removeProperty(any a0);
};
