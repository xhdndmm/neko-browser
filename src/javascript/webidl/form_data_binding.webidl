// Binding surface extracted from src/javascript/src/binding/form_data_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=proto, LocalProto]
interface FormDataBindingProto {
  [CFunction=FormDataAppend] void append(any a0, any a1);
  [CFunction=FormDataSet] void set(any a0, any a1);
  [CFunction=FormDataDelete] void delete(any a0);
  [CFunction=FormDataGet] void get(any a0);
  [CFunction=FormDataHas] void has(any a0);
  [CFunction=FormDataEntries] void entries();
};
