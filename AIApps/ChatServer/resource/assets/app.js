/* CppAI 前端公共工具：Toast 通知（替代原生 alert） */
(function () {
    'use strict';

    function ensureRoot() {
        var root = document.getElementById('toast-root');
        if (!root) {
            root = document.createElement('div');
            root.id = 'toast-root';
            document.body.appendChild(root);
        }
        return root;
    }

    window.cppaiToast = function (message, type, duration) {
        type = type || 'info';
        duration = duration || 2200;
        var root = ensureRoot();
        var el = document.createElement('div');
        el.className = 'toast ' + type;
        el.textContent = message;
        root.appendChild(el);
        setTimeout(function () {
            el.classList.add('hide');
            setTimeout(function () { el.remove(); }, 320);
        }, duration);
    };
})();
