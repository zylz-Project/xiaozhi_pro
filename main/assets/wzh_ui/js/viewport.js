/* Stop Safari's page-zoom gesture without interfering with normal scrolling. */
(function () {
  'use strict';
  ['gesturestart', 'gesturechange'].forEach(function (name) {
    document.addEventListener(name, function (event) { event.preventDefault(); }, { passive: false });
  });
})();
