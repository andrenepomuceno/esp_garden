function fillTable(id, data, mode) {
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
  $("#input-watering").prop("checked", (info["Outputs"]["Watering"] == 1) ? true : false);
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

  /*let info = {"Status":{"Hostname":"espgarden1","Date/Time":"2023-01-08 16:11:59","Uptime":"0d 0h 5m 19s","Internet":"online","Signal Strength":"100%","ThingSpeak":"enabled","Packages Sent":"2","Watering Cycles":"0","DHT Read Errors":"3"},"Inputs":{"Soil Moisture":{"val":"41.27","avg":"41.70","var":"0.03"},"Luminosity":{"val":"53.09","avg":"52.65","var":"0.02"},"Temperature":{"val":"26.70","avg":"26.63","var":"0.01"},"Air Humidity":{"val":"82.00","avg":"82.33","var":"0.14"}},"Outputs":{"Watering":"0"},"Channel":"1348790"};
  updateUI(info);*/

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

$("#input-watering").click(function (event) {
  event.preventDefault();
  let wateringTime = $("#input-watering-time").val();
  if (this.checked && confirm("Start watering for " + wateringTime + " seconds?")) {
    $.post("/control", { "wateringTime": 1000 * wateringTime });
  }
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