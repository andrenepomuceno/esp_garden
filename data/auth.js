// ESP Garden — session token plumbing shared by every page.
(function (global) {
  var KEY = 'espgarden.token';
  var LOGIN_PAGE = '/login.html';

  function getToken() {
    return localStorage.getItem(KEY) || sessionStorage.getItem(KEY) || '';
  }

  function setToken(token, persist) {
    clearToken();
    (persist ? localStorage : sessionStorage).setItem(KEY, token);
  }

  function clearToken() {
    localStorage.removeItem(KEY);
    sessionStorage.removeItem(KEY);
  }

  function onLoginPage() {
    return global.location.pathname === LOGIN_PAGE;
  }

  function redirectToLogin() {
    if (!onLoginPage()) global.location.href = LOGIN_PAGE;
  }

  function requireToken() {
    if (!getToken() && !onLoginPage()) {
      redirectToLogin();
      return false;
    }
    return true;
  }

  if (global.jQuery) {
    // A prefilter, not ajaxSetup({beforeSend}): a per-call beforeSend (update.js
    // installs one for upload progress) replaces the global one outright, and
    // the header would then silently go missing on exactly the request that
    // needs ADMIN.
    global.jQuery.ajaxPrefilter(function (options) {
      var token = getToken();
      if (!token) return;
      options.headers = options.headers || {};
      options.headers['Authorization-Token'] = token;
    });

    global.jQuery(document).ajaxError(function (event, jqXHR) {
      if (jqXHR.status === 401) {
        clearToken();
        redirectToLogin();
      }
    });
  }

  global.espAuth = {
    getToken: getToken,
    setToken: setToken,
    clearToken: clearToken,
    requireToken: requireToken,
    redirectToLogin: redirectToLogin,
    logout: function () {
      global.jQuery
        .post('/logout')
        .always(function () {
          clearToken();
          redirectToLogin();
        });
    },
  };
})(window);
