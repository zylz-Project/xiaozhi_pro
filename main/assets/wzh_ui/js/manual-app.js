(function () {
  var q = new URLSearchParams(location.search);
  var theme = q.get('theme'), mode = q.get('mode');
  if (!theme && !mode) return;
  Array.prototype.forEach.call(document.querySelectorAll('.phone'), function (phone) {
    if (theme) phone.setAttribute('data-theme', theme);
    if (mode === 'dark') phone.classList.add('dark');
    if (mode === 'light') phone.classList.remove('dark');
  });
})();
