const mqtt = require('mqtt');
const client = mqtt.connect('mqtt://103.179.25.52:1883');
client.on('connect', () => {
    console.log('Connected! Turning OFF...');
    const payload = JSON.stringify({ command: "OFF" });
    client.publish('device/3C8A1F9AC404/command', payload, () => {
        console.log('Sent OFF command');
        client.end();
    });
});
