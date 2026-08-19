function setStatus(kind, message) {
    var classes = {
        info: 'alert alert-info',
        success: 'alert alert-success',
        danger: 'alert alert-danger',
    };
    $('#status').html('<div class="' + (classes[kind] || classes.info) + '">' + message + '</div>');
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
                setStatus('danger', 'Could not enable OTA: ' + (errorThrown || textStatus));
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
});
