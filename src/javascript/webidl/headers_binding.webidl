// Binding surface extracted from src/javascript/src/binding/headers_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=prototype, LocalProto]
interface HeadersBindingPrototype {
  [CFunction=Append] void append(any a0, any a1);
  [CFunction=Set] void set(any a0, any a1);
  [CFunction=Get] void get(any a0);
  [CFunction=Has] void has(any a0);
  [CFunction=Delete] void delete(any a0);
  [CFunction=Entries] void entries();
};
