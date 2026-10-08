(function() {
    const btnLofi = document.getElementById("btn-study-lofi-toggle");
    const btnWn = document.getElementById("btn-study-wn-toggle");
    const badgeLofi = document.getElementById("study-lofi-badge");
    const badgeWn = document.getElementById("study-wn-badge");

    if (btnLofi) {
        btnLofi.onclick = async () => {
            const isLofi = btnLofi.classList.contains("active");
            btnLofi.disabled = true;
            try {
                if (isLofi) {
                    await sendCommand("study/lofi/stop");
                } else {
                    await sendCommand("study/lofi/start");
                }
                await fetchStatus();
            } finally {
                btnLofi.disabled = false;
            }
        };
    }

    if (btnWn) {
        btnWn.onclick = async () => {
            const isWn = btnWn.classList.contains("active");
            btnWn.disabled = true;
            try {
                if (isWn) {
                    await sendCommand("study/whitenoise/stop");
                } else {
                    await sendCommand("study/whitenoise/start");
                }
                await fetchStatus();
            } finally {
                btnWn.disabled = false;
            }
        };
    }

    function onStatusUpdate(e) {
        const s = e.detail;
        if (!s) return;
        if (badgeLofi && btnLofi) {
            badgeLofi.style.display = s.study_lofi_active ? "inline-block" : "none";
            if (s.study_lofi_active) {
                btnLofi.classList.add("active");
                btnLofi.style.borderColor = "#a6e3a1";
            } else {
                btnLofi.classList.remove("active");
                btnLofi.style.borderColor = "";
            }
        }
        if (badgeWn && btnWn) {
            badgeWn.style.display = s.study_whitenoise_active ? "inline-block" : "none";
            if (s.study_whitenoise_active) {
                btnWn.classList.add("active");
                btnWn.style.borderColor = "#94e2d5";
            } else {
                btnWn.classList.remove("active");
                btnWn.style.borderColor = "";
            }
        }
    }

    window.addEventListener("cdplayer:status", onStatusUpdate);
    if (window.currentStatus) {
        onStatusUpdate({ detail: window.currentStatus });
    }
})();
