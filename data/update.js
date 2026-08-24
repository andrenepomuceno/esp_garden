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

function uploadFile(formData) {
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
                    }
                }, false);
            }
            return xhr;
        },
        beforeSend: function () {
            $('#progress-card').show();
            setProgress(0);
            setStatus('info', 'Uploading...');
        },
        success: function () {
            setProgress(100);
            setStatus('success', 'Upload successful! Device will restart...');
            $('#button-upload').html('&#x21A9; Done').prop('disabled', false);
        },
        error: function (jqXHR, textStatus, errorThrown) {
            var msg = jqXHR.responseText || errorThrown || textStatus || 'Unknown error';
            setStatus('danger', 'Upload failed: ' + msg);
            $('#button-upload').html('&#x21A9; Back').prop('disabled', false);
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

$(function () {
    // OTA is ADMIN-only; without a token every request here answers 401.
    if (!espAuth.requireToken()) return;

    $('#file-input').on('change', function () {
        var file = this.files[0];
        if (!file) return;
        var name = file.name.toLowerCase();
        if (name === 'firmware.bin') {
            $('#type-firmware').prop('checked', true);
        } else if (name === 'spiffs.bin' || name === 'filesystem.bin') {
            $('#type-filesystem').prop('checked', true);
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
        if (path.length > 31) {
            $('#progress-card').show();
            setStatus('danger', 'Path is ' + path.length +
                                ' characters; SPIFFS allows 31.');
            return;
        }

        $('#path-single').val(path);
        $(this).prop('disabled', true);
        startSingleFileUpload(file, path);
    });
});
