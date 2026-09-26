const mqtt = require("mqtt");
const client = mqtt.connect(["mqtt://test1", "mqtt://test2"]);
console.log(client.options.hostname);
client.end();
