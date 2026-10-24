// Binding surface extracted from src/javascript/src/binding/url_search_params_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=proto, LocalProto]
interface UrlSearchParamsBindingProto {
  [CFunction=Append] void append(any a0, any a1);
  [CFunction=Set] void set(any a0, any a1);
  [CFunction=Delete] void delete(any a0);
  [CFunction=Get] void get(any a0);
  [CFunction=GetAll] void getAll(any a0);
  [CFunction=Has] void has(any a0);
  [CFunction=Sort] void sort();
  [CFunction=ToString] void toString();
  [Getter=Size] readonly attribute any size;
};
