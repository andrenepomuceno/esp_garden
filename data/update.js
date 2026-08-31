// Shared with config.js via auth.js: one alert renderer, one escaping rule.
// Device response bodies land here and must not be parsed as HTML.
function setStatus(kind, message) {
    espUI.setStatus(kind, message);
}

function setProgress(percent) {
    var p = Math.max(0, Math.min(100, Math.round(percent)));
    $('#progress-bar').css('width', p + '%').text(p + '%');
}

function resetUI() {
    $('#progress-card').hide();
    setProgress(0);
    $('#status').html('');
    $('#button-upload').html('&#x1F680; Upload').prop('disabled', false);
}

// The firmware the device reported before the upload started, so the outcome
// can be judged against what actually changed.
var versionBefore = null;

function readVersion() {
    return $.getJSON('/data.json').then(function (info) {
        return (info && info.Status && info.Status.Firmware) || null;
    });
}

// THE UPLOAD'S RETURN VALUE IS NOT THE VERDICT.
//
// A finished flash ends in a reboot, and the reboot kills the connection the
// browser is still waiting on — so a successful update arrives here as an
// error. That is not hypothetical: an operator watched this page report
// "Upload failed: error" at 100 % three times while the firmware on the board
// had in fact changed. The device is the only thing that knows, so ask it.
//
// It may refuse to answer. Sessions live in RAM unless the login was made with
// "remember", so a reboot usually signs the browser out and /data.json returns
// 401. That is still information: the device is up, it restarted, and the page
// says exactly that rather than inventing an outcome.
function confirmOutcome(reachedEnd) {
    var deadline = Date.now() + 120000;
    var sawItGo = false;

    setStatus('info', reachedEnd
        ? 'Upload complete. Waiting for the device to come back...'
        : 'Connection lost. Checking whether the device took the update...');

    function poll() {
        if (Date.now() > deadline) {
            setStatus('danger', reachedEnd
                ? 'The whole image was sent, but the device never reported a ' +
                  'new version. Check /logs, then try again.'
                : 'The upload did not complete and the device did not change ' +
                  'firmware. Nothing was flashed; try again.');
            $('#button-upload').html('&#x21A9; Back').prop('disabled', false);
            return;
        }

        readVersion().done(function (now) {
            if (!now) return setTimeout(poll, 3000);
            if (versionBefore && now !== versionBefore) {
                setStatus('success', 'Updated. The device is running ' + now + '.');
                $('#button-upload').html('&#x21A9; Done').prop('disabled', false);
                return;
            }
            // Same version answering again. For a FILESYSTEM image that is the
            // expected outcome — the firmware does not change — so a reboot we
            // watched happen is the only evidence available.
            if (sawItGo) {
                setStatus('success', 'The device restarted and is answering on ' +
                    now + '. A filesystem update does not change the ' +
                    'firmware version, so this is what success looks like for one.');
                $('#button-upload').html('&#x21A9; Done').prop('disabled', false);
                return;
            }
            setTimeout(poll, 3000);
        }).fail(function (jqXHR) {
            // Unreachable, or signed out by the reboot. Both mean it went away.
            sawItGo = true;
            if (jqXHR && jqXHR.status === 401) {
                setStatus('warning', 'The device restarted, which signed this ' +
                    'browser out. Sign in again to confirm the version — the ' +
                    'update itself most likely succeeded.');
                $('#button-upload').html('&#x21A9; Back').prop('disabled', false);
                return;
            }
            setTimeout(poll, 3000);
        });
    }

    setTimeout(poll, 4000);
}

function uploadFile(formData) {
    var reachedEnd = false;

    $.ajax({
        url: '/update',
        type: 'POST',
        data: formData,
        processData: false,
        contentType: false,
        xhr: function () {
            var xhr = $.ajaxSettings.xhr();
            if (xhr.upload) {
                xhr.upload.addEventListener('progress', function (evt) {
                    if (evt.lengthComputable) {
                        setProgress((evt.loaded / evt.total) * 100);
                        // Whether the whole body left the browser is the one
                        // thing this side genuinely knows, and it separates a
                        // reboot that ate the reply from an upload that died.
                        if (evt.loaded >= evt.total) reachedEnd = true;
                    }
                }, false);
            }
            return xhr;
        },
        beforeSend: function () {
            $('#progress-card').show();
            setProgress(0);
            setStatus('info', 'Uploading...');
            $('#button-upload').prop('disabled', true);
        },
        success: function () {
            setProgress(100);
            confirmOutcome(true);
        },
        error: function (jqXHR, textStatus, errorThrown) {
            // A 4xx from the device is a real refusal with a real reason, and
            // it is worth reporting verbatim: the device is still up, so there
            // is nothing to confirm.
            if (jqXHR && jqXHR.status >= 400 && jqXHR.status < 500) {
                var msg = jqXHR.responseText || errorThrown || textStatus;
                setStatus('danger', 'Refused by the device: ' + msg);
                $('#button-upload').html('&#x21A9; Back').prop('disabled', false);
                return;
            }
            confirmOutcome(reachedEnd);
        },
    });
}

function startUpload(file, updateType) {
    setStatus('info', 'Calculating MD5 hash...');
    $('#progress-card').show();
    setProgress(0);

    var reader = new FileReader();
    reader.onload = function (e) {
        var spark = new SparkMD5.ArrayBuffer();
        spark.append(e.target.result);
        var md5Hash = spark.end();

        var renamedFile = new File([file], updateType, { type: file.type });
        var formData = new FormData();
        formData.append('MD5', md5Hash);
        formData.append('file', renamedFile);

        $.ajax({
            url: '/updateEnable',
            type: 'POST',
            success: function () { uploadFile(formData); },
            error: function (jqXHR, textStatus, errorThrown) {
                // The BODY, not the status text. The firmware answers 409 with
                // "Zona 2 is running. Updating reboots the device, and a relay
                // is energised across a reset." — naming the relay and the
                // reason. Rendering "Conflict" instead throws that away and
                // leaves the operator retrying into the same refusal.
                var msg = jqXHR.responseText || errorThrown || textStatus ||
                          'Unknown error';
                setStatus('danger', 'Could not enable OTA: ' + msg);
                $('#button-upload').html('&#x21A9; Back').prop('disabled', false);
            },
        });
    };
    reader.onerror = function () {
        setStatus('danger', 'Failed to read file.');
        $('#button-upload').html('&#x21A9; Back').prop('disabled', false);
    };
    reader.readAsArrayBuffer(file);
}

// Replaces one file rather than the whole partition. Same MD5 discipline as
// the firmware path: the device compares what landed against this hash before
// moving the file into place, so a truncated upload fails loudly instead of
// silently replacing a working asset with half of one.
function startSingleFileUpload(file, path) {
    setStatus('info', 'Calculating MD5 hash...');
    $('#progress-card').show();
    setProgress(0);

    var reader = new FileReader();
    reader.onload = function (e) {
        var spark = new SparkMD5.ArrayBuffer();
        spark.append(e.target.result);

        var formData = new FormData();
        formData.append('MD5', spark.end());
        // The device reads the destination from the multipart filename, the
        // same way it tells firmware from filesystem on /update.
        formData.append('file', new File([file], path, { type: file.type }));

        $.ajax({
            url: '/spiffs/upload',
            type: 'POST',
            data: formData,
            processData: false,
            contentType: false,
            xhr: function () {
                var xhr = $.ajaxSettings.xhr();
                if (xhr.upload) {
                    xhr.upload.addEventListener('progress', function (evt) {
                        if (evt.lengthComputable) {
                            setProgress((evt.loaded / evt.total) * 100);
                        }
                    }, false);
                }
                return xhr;
            },
            beforeSend: function () { setStatus('info', 'Uploading ' + path + '...'); },
            success: function (result) {
                setProgress(100);
                // No restart: the file is live for the next request that asks
                // for it, which is the whole point of not rewriting the image.
                setStatus('success', 'Wrote ' + result.path + ' (' + result.bytes +
                                     ' bytes). ' + result.free + ' bytes free. ' +
                                     'No restart needed.');
                $('#button-single').prop('disabled', false);
            },
            error: function (jqXHR, textStatus, errorThrown) {
                var msg = errorThrown || textStatus || 'Unknown error';
                if (jqXHR.responseJSON && jqXHR.responseJSON.error) {
                    msg = jqXHR.responseJSON.error;
                } else if (jqXHR.responseText) {
                    msg = jqXHR.responseText;
                }
                setStatus('danger', 'Upload failed: ' + msg);
                $('#button-single').prop('disabled', false);
            },
        });
    };
    reader.onerror = function () {
        setStatus('danger', 'Failed to read file.');
        $('#button-single').prop('disabled', false);
    };
    reader.readAsArrayBuffer(file);
}

// The device owns this rule; /capabilities.json publishes it. The literal is
// the fallback for a device too old to answer, so a failed lookup costs a
// slightly stale limit rather than a broken upload page.
var maxPathLength = 31;

$(function () {
    // OTA is ADMIN-only; without a token every request here answers 401.
    if (!espAuth.requireToken()) return;

    $.getJSON('/capabilities.json')
        .done(function (caps) {
            if (caps && typeof caps.maxPathLength === 'number') {
                maxPathLength = caps.maxPathLength;
            }
        });

    // Read before any upload starts: the outcome is judged by what changed.
    readVersion().done(function (v) { versionBefore = v; });

    $('#file-input').on('change', function () {
        var file = this.files[0];
        if (!file) return;
        var name = file.name.toLowerCase();
        if (name === 'firmware.bin') {
            $('#type-firmware').prop('checked', true);
        } else if (name === 'littlefs.bin' || name === 'spiffs.bin' ||
                   name === 'filesystem.bin') {
            $('#type-filesystem').prop('checked', true);
        } else {
            // An unrecognised name is the dangerous case, not a harmless one:
            // leaving the previous radio checked means a filesystem image
            // picked while "firmware" is still ticked gets renamed to
            // `firmware`, and handleUpdateUpload decides U_FLASH from that
            // name. The image lands in the app slot, the upload reports
            // success, and the board is unbootable until someone reaches it
            // with USB. littlefs.bin is what the build produces now; spiffs.bin
            // stays recognised because an operator may still have one on disk.
            $('#type-firmware').prop('checked', false);
            $('#type-filesystem').prop('checked', false);
        }
    });

    $('#button-upload').on('click', function () {
        var button = $('#button-upload');
        var label = button.text().trim();

        if (label.indexOf('Back') !== -1 || label.indexOf('Done') !== -1) {
            resetUI();
            return;
        }

        var fileInput = document.getElementById('file-input');
        var file = fileInput.files[0];
        if (!file) {
            setStatus('danger', 'Please select a .bin file to upload.');
            $('#progress-card').show();
            return;
        }

        var updateType = $('input[name="updateType"]:checked').val();
        if (!updateType) {
            setStatus('danger', 'Please select an update type.');
            $('#progress-card').show();
            return;
        }

        button.prop('disabled', true).html('&#x21A9; Back');
        startUpload(file, updateType);
        button.prop('disabled', false);
    });

    // Prefill the destination from the chosen file, since replacing an asset
    // in place is the common case and retyping the name is how a typo lands a
    // file the server will never serve.
    $('#file-single').on('change', function () {
        var file = this.files[0];
        if (file && !$('#path-single').val()) {
            $('#path-single').val('/' + file.name);
        }
    });

    $('#button-single').on('click', function () {
        var file = document.getElementById('file-single').files[0];
        if (!file) {
            $('#progress-card').show();
            setStatus('danger', 'Please select a file to upload.');
            return;
        }

        var path = ($('#path-single').val() || ('/' + file.name)).trim();
        if (path.charAt(0) !== '/') path = '/' + path;

        // Checked here as well as on the device, so the answer is immediate
        // and does not cost a full upload to find out.
        if (path.length > maxPathLength) {
            $('#progress-card').show();
            setStatus('danger', 'Path is ' + path.length +
                                ' characters; the device allows ' +
                                maxPathLength + '.');
            return;
        }

        $('#path-single').val(path);
        $(this).prop('disabled', true);
        startSingleFileUpload(file, path);
    });
});
