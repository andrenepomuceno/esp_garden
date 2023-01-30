function fillTable(id, data, mode) {
    if (data == {} || data == undefined) return
    var i = 0;
    var tbody = $(id);
    tbody.empty();
    for (var key in data) {
      var tr = $('<tr/>');
      if (mode != 'status') tr.append($('<th/>').html(i++).attr("scope", "row"));
      tr.append($('<td/>').html(key));
      if (mode == 'inputs') {
        tr.append($('<td/>').html(data[key].val.toFixed(3)));
        tr.append($('<td/>').html(data[key].avg.toFixed(3)));
        tr.append($('<td/>').html(data[key].var.toFixed(3)));
      } else if (mode == 'outputs') {
        tr.append($('<td/>').html(data[key]));
      } else {
        tr.append($('<td/>').html(data[key]));
      }
      tbody.append(tr);
    }
  }

function updateUI(info) {
  document.title = info["Status"]["Hostname"];
  fillTable("#tbody-status", info["Status"], 'status');
  fillTable("#tbody-inputs", info["Inputs"], 'inputs');
  fillTable("#tbody-outputs", info["Outputs"], 'outputs');
  $("#input-mqtt").prop("checked", (info["Status"]["MQTT"] == "enabled") ? true : false);
  $("#a-thingspeak-link").attr("href", "https://thingspeak.com/channels/" + info["Channel"]);
}

function refresh() {
  setTimeout(refresh, 500);

  $.ajax({
    dataType: "json",
    url: "/data.json",
    timeout: 250,
    success: updateUI
  });

  $.ajax({
    url: "/logs",
    timeout: 250,
    success: function (log) {
      $("#textarea-logs").text(log);
      if ($("#input-scroll").prop("checked") == true) {
        $("#textarea-logs").scrollTop($("#textarea-logs")[0].scrollHeight);
      }
    }
  });
}

$(function onReady() {
  refresh();
});

$("#input-mqtt").click(function (event) {
  event.preventDefault();
  if (this.checked) {
    $.post("/control", { "mqtt": "enable" });
  } else {
    $.post("/control", { "mqtt": "disable" });
  }
});

$("#button-reset").click(function (event) {
  event.preventDefault();
  if (confirm("Reset now?")) {
    $.post("/control", { "reset": "1" });
  }
});