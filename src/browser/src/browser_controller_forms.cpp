// BrowserController implementation: forms.
#include "browser_controller_internal.h"

namespace neko::browser {

void BrowserController::SubmitForm(int tab_id, dom::Element* form, dom::Element* submitter)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr || form == nullptr || tab->page == nullptr) {
    return;
  }
  // Keep the Page alive while the submission runs: a scripted submission (a
  // form.submit() issued from a submit/invalid handler) replaces the document
  // before this call returns.
  const std::shared_ptr<renderer::Page> page_lock_keepalive = tab->page;
  const std::string document_url = tab->url;
  const bool navigated =
      RunFormSubmission(*tab, *page_lock_keepalive, document_url, form, submitter, false);
  // A navigation replaces the document, invalidating every queued element
  // pointer; only drain while the submitting document is still current.
  if (!navigated) {
    DrainPendingFormSubmission(*tab);
  }
}

bool BrowserController::RunFormSubmission(Tab& tab,
                                          renderer::Page& page,
                                          const std::string& document_url,
                                          dom::Element* form,
                                          dom::Element* submitter,
                                          bool scripted)
{
  FormSubmissionRequest request{form, submitter, scripted};
  // A form.submit()/requestSubmit() issued by a handler during this submission
  // supersedes it (the spec's planned navigation).  Returns true when a newer
  // request was taken over.
  const auto supersede = [&tab, &request]() -> bool {
    if (tab.pending_form_submission.form == nullptr) {
      return false;
    }
    request.form = tab.pending_form_submission.form;
    request.submitter = tab.pending_form_submission.submitter;
    request.scripted = !tab.pending_form_submission.request_submit;
    tab.pending_form_submission = {};
    return true;
  };
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (request.form == nullptr) {
      return false;
    }
    const dom::Document* document = page.document();
    if (document == nullptr || !ContainsNode(*document, request.form) ||
        (request.submitter != nullptr && !ContainsNode(*document, request.submitter))) {
      // The element was removed by a script between the request and its
      // execution.  Dropping the request is the only safe option: the pointer
      // is no longer part of the live tree.
      return false;
    }
    std::optional<PreparedFormSubmission> prepared;
    {
      const std::unique_lock<std::recursive_mutex> dom_lock = page.AcquireDomLock();
      if (!request.scripted) {
        // Interactive validation comes before the submit event (HTML
        // §4.10.22.3).  A scripted submission during either phase supersedes
        // this one.
        if (supersede()) {
          continue;
        }
        const bool no_validate =
            request.form->HasAttribute("novalidate") ||
            (request.submitter != nullptr && request.submitter->HasAttribute("formnovalidate"));
        if (!no_validate) {
          std::vector<dom::Element*> invalid;
          if (tab.script_runtime != nullptr) {
            invalid = tab.script_runtime->InvalidFormControls(*request.form);
            for (dom::Element* control : invalid) {
              tab.script_runtime->DispatchInvalidEvent(*control);
            }
          } else {
            invalid = html::InvalidControls(*request.form);
          }
          if (!invalid.empty()) {
            // Interactive validation reports the problem at the first invalid
            // control (focus; the UI paints the caret there).
            FocusControl(&tab, invalid.front(), &page);
            if (supersede()) {
              continue;
            }
            return false;
          }
        }
        if (supersede()) {
          continue;
        }
        if (tab.script_runtime != nullptr &&
            !tab.script_runtime->DispatchCancelableEvent(*request.form, "submit")) {
          // preventDefault canceled this submission.  A form.submit() issued
          // by the handler still navigates (submit() skips the event).
          if (supersede()) {
            continue;
          }
          return false;
        }
        if (supersede()) {
          continue;
        }
      }
      PreparedFormSubmission candidate;
      if (!PrepareFormSubmission(document_url, *request.form, request.submitter, &candidate)) {
        return false;
      }
      // No script runs between the last supersede check and here, so the
      // prepared submission is still the newest one.
      prepared = std::move(candidate);
    }
    return ApplyFormSubmission(tab, std::move(*prepared));
  }
  // A pathological submit-handler loop: drop anything still queued.
  tab.pending_form_submission = {};
  return false;
}

bool BrowserController::PrepareFormSubmission(const std::string& document_url,
                                              dom::Element& form,
                                              dom::Element* submitter,
                                              PreparedFormSubmission* out)
{
  // Attributes for form submission: the submitter's form* attributes win over
  // the form's (HTML §4.10.19.6).
  const auto attr_or_form = [&form, submitter](std::string_view submitter_attr,
                                               std::string_view form_attr) -> std::string {
    if (submitter != nullptr && submitter->HasAttribute(submitter_attr)) {
      return std::string(submitter->GetAttribute(submitter_attr).value_or(""));
    }
    return std::string(form.GetAttribute(form_attr).value_or(""));
  };

  // The action of the element: submitter's formaction, else the form's action.
  std::string action = attr_or_form("formaction", "action");
  std::string target = ResolveUrlAgainstBase(action, document_url);
  if (target.empty()) {
    target = document_url;
  }

  // Method: enumerated attribute.  get/post are implemented; dialog is
  // explicitly NOT IMPLEMENTED (there is no dialog element/close algorithm),
  // so it falls back to GET rather than silently posting.
  const std::string method_value =
      base::ToLower(std::string(Trim(attr_or_form("formmethod", "method"))));
  const bool post = method_value == "post";

  // Enctype: submitter's formenctype, else the form's enctype (invalid/missing
  // value default: application/x-www-form-urlencoded).
  const FormEnctype enctype = ParseEnctype(attr_or_form("formenctype", "enctype"));

  std::vector<html::FormEntry> entries = html::ConstructEntryList(form, submitter);
  std::string body;
  std::string content_type;
  if (post) {
    out->method = "POST";
    switch (enctype) {
    case FormEnctype::kMultipart: {
      const std::string boundary = GenerateMultipartBoundary();
      body = EncodeMultipart(entries, boundary);
      content_type = "multipart/form-data; boundary=" + boundary;
      break;
    }
    case FormEnctype::kTextPlain:
      body = EncodePlainText(entries);
      content_type = "text/plain";
      break;
    case FormEnctype::kUrlEncoded:
      body = EncodeUrlEncoded(entries);
      content_type = "application/x-www-form-urlencoded";
      break;
    }
  } else {
    // GET ("mutate action URL", including the dialog fallback): the entry list
    // always travels as an application/x-www-form-urlencoded query string,
    // whatever the enctype says.
    out->method.clear();
    target = AppendQuery(std::move(target), EncodeUrlEncoded(entries));
  }

  out->url = std::move(target);
  out->body = std::move(body);
  out->content_type = std::move(content_type);
  out->target = std::string(Trim(attr_or_form("formtarget", "target")));
  // Password manager: capture the login this submission would offer to save
  // (the origin gate below skips file:/data: documents).
  out->credential = CredentialFromForm(form);
  return true;
}

bool BrowserController::ApplyFormSubmission(Tab& tab, PreparedFormSubmission submission)
{
  // Publish the captured login (if any) before navigating: the save prompt
  // belongs to this submission, not to whatever page comes next.
  if (submission.credential.has_value()) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tab.origin.empty() && tab.origin != "null") {
      submission.credential->origin = tab.origin;
      tab.pending_credential = std::move(submission.credential);
    }
  }
  // Navigate outside the DOM lock.  NavigateWithRequestBody() replaces the
  // tab's Page, which destroys the Page the lock was taken on; unlocking it
  // afterwards was a use-after-free on the destroyed Page's mutex (reported by
  // ThreadSanitizer, and undefined behaviour per the standard).
  //
  // _blank opens a new tab through the normal navigation path (history and
  // cookies stay browser-side).  _self, _parent and _top all map to the
  // current tab, and a named target falls back to it as well: the engine has no
  // frame tree / named browsing contexts yet (documented approximation).
  const std::string target_keyword = base::ToLower(submission.target);
  if (target_keyword == "_blank") {
    const int new_tab = NewTab();
    static_cast<void>(NavigateWithRequestBody(new_tab,
                                              submission.url,
                                              std::move(submission.method),
                                              std::move(submission.body),
                                              std::move(submission.content_type)));
    return false;
  }
  static_cast<void>(NavigateWithRequestBody(tab.id,
                                            submission.url,
                                            std::move(submission.method),
                                            std::move(submission.body),
                                            std::move(submission.content_type)));
  return true;
}

void BrowserController::DrainPendingFormSubmission(Tab& tab)
{
  for (int attempt = 0; attempt < 8; ++attempt) {
    const ScriptRequestedFormSubmission request = tab.pending_form_submission;
    tab.pending_form_submission = {};
    if (request.form == nullptr) {
      return;
    }
    const std::shared_ptr<renderer::Page> page = tab.page;
    if (page == nullptr) {
      return;
    }
    const std::string document_url = tab.url;
    const bool navigated = RunFormSubmission(
        tab, *page, document_url, request.form, request.submitter, !request.request_submit);
    // Any navigation replaces the document (and invalidates the queued
    // pointers); stop rather than interpreting stale state.
    if (navigated || tab.page != page) {
      return;
    }
  }
  tab.pending_form_submission = {};
}

} // namespace neko::browser
