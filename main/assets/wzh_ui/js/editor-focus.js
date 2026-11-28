(function () {
  'use strict';
  const fields = '[data-edit-on-tap]';

  function enableKeyboard(event) {
    if (event.isTrusted && event.target.matches?.(fields)) {
      event.target.inputMode = 'text';
    }
  }

  function blurWithin(container) {
    const field = document.activeElement;
    if (field && container.contains(field) &&
        (field.matches('input, textarea, select') || field.isContentEditable)) {
      field.blur();
    }
  }

  // Set the input mode before the browser focuses a tapped field. Click also
  // covers activation through assistive technology.
  document.addEventListener('pointerdown', enableKeyboard, true);
  document.addEventListener('click', enableKeyboard, true);
  document.addEventListener('focusout', event => {
    if (event.target.matches(fields)) event.target.inputMode = 'none';
  });

  // Do not carry a text editor into a history entry, including Safari's cache.
  window.addEventListener('pagehide', () => blurWithin(document));
  window.addEventListener('pageshow', event => {
    if (event.persisted) blurWithin(document);
  });

  window.WzhEditorFocus = {blurWithin};
})();
