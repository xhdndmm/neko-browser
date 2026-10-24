// WebIDL source for the generated part of HTMLElement.
//
// This is a subset of the WHATWG HTML HTMLElement interface: only members
// whose binding is a plain content-attribute reflection or a hand-written
// method registration.  Members with engine-specific semantics (dir,
// tabIndex, className, ...) stay hand-written in element_binding.cpp; they
// are added here once the generator can express their behaviour.
//
// The grammar and the meaning of the extended attributes are documented in
// tools/webidl/gen.py.

[Exposed=Window, Proto=element_proto]
interface HTMLElement : Element {
  // HTML §3.2.6.5: title/lang are plain DOMString reflections.
  [Reflect=title] attribute DOMString title;
  [Reflect=lang] attribute DOMString lang;

  // HTML §6.6.1 / the rendering section: hidden and inert are boolean
  // content-attribute reflections.
  [Reflect=hidden] attribute boolean hidden;
  [Reflect=inert] attribute boolean inert;

  // The body of focus() is hand-written (focus controller lives in the
  // browser layer); the generator owns its registration.
  [CFunction=ElementFocus] void focus();
};
