// Binding surface extracted from src/javascript/src/binding/storage_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=prototype, LocalProto]
interface StorageBindingPrototype {
  [CFunction=ResponseText] void text();
  [CFunction=ResponseJson] void json();
};
