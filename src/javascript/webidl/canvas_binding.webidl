// Binding surface extracted from src/javascript/src/binding/canvas_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=canvas_2d_proto]
interface CanvasBindingCanvas2dProto {
  [Getter=CanvasGetFillStyle, Setter=CanvasSetFillStyle] attribute any fillStyle;
};
