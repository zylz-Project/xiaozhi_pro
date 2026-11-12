(function () {
  var params = new URLSearchParams(location.search);
  if (params.get("view") !== "expanded") return;
  setTimeout(function () {
    var phone = document.querySelector(".phone");
    if (phone && phone.__setExpanded) phone.__setExpanded(true);
  }, 420);
})();
