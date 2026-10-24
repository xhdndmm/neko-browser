// Binding surface extracted from src/javascript/src/binding/platform_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=encoder_proto, LocalProto]
interface PlatformBindingEncoderProto {
  [Getter=EncodingGetterImpl] readonly attribute any encoding;
};

[Proto=decoder_proto, LocalProto]
interface PlatformBindingDecoderProto {
  [Getter=EncodingGetterImpl] readonly attribute any encoding;
};
