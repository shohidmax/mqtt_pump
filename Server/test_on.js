const mqtt = require('mqtt');
const client = mqtt.connect('mqtt://103.179.25.52:1883');
client.on('connect', () => {
    console.log('Connected! Turning ON (triggering RELAY_1)...');
    const payload = JSON.stringify({ type: "command", command: "RELAY_1" });
    client.publish('device/3C8A1F9AC404/command', payload, () => {
        console.log('Sent ON command');
        client.end();
    });
});
