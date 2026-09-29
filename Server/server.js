require('dotenv').config();
const WebSocket = require('ws');
const http = require('http');
const express = require('express');
const path = require('path');
const mongoose = require('mongoose');
const mqtt = require('mqtt');

const app = express();
const server = http.createServer(app);

// --- MongoDB Setup ---
const MONGODB_URI = process.env.MONGODB_URI || "mongodb://localhost:27017/motor_data";
mongoose.connect(MONGODB_URI)
    .then(() => console.log('MongoDB Connected'))
    .catch(err => console.error('MongoDB connection error:', err));

const LogSchema = new mongoose.Schema({
    macAddress: String,
    startTime: Date,
    endTime: Date,
    duration: String,
    bdDate: String,
    bdTime: String,
    createdAt: { type: Date, default: Date.now, expires: 7776000 } // 90 Days TTL
});
const MotorLog = mongoose.model('MotorLog4', LogSchema);

// --- State Tracking ---
let motorStartTime = null;
let lastMotorStatus = 'OFF';
const DEVICE_MAC = "3C8A1F9AC404"; // Updated to user's ESP32 MAC
const webClients = new Set();
let isEspOnline = false;

const wss = new WebSocket.Server({ server });

// --- MQTT Consumer Setup (HA Architecture) ---
const MQTT_BROKER_1 = process.env.MQTT_PRIMARY || 'wss://mosquitto-muthosech.espserver.site:443';
const MQTT_BROKER_2 = process.env.MQTT_SECONDARY || 'wss://mosquitto-muthosech.espserver.site:443';

const mqttClient = mqtt.connect(MQTT_BROKER_1, {
    clientId: 'nodejs_consumer_' + Math.random().toString(16).substr(2, 8),
    clean: true,
    connectTimeout: 4000,
    reconnectPeriod: 1000
});

mqttClient.on('connect', () => {
    console.log('Connected to MQTT Broker Cluster');
    mqttClient.subscribe('device/+/status', { qos: 1 });
    
    // We assume ESP is online if we are connected, but we can also use LWT (Last Will) in future
});

mqttClient.on('message', (topic, message) => {
    try {
        const payloadStr = message.toString();
        const data = JSON.parse(payloadStr);

        if (data.type === 'statusUpdate') {
            isEspOnline = true; // Received data means it's online
            
            // Extract MAC from topic (e.g. device/3C8A1F9AC404/status)
            const parts = topic.split('/');
            const mac = parts[1];
            data.macAddress = mac;
            const payloadStrOut = JSON.stringify(data);

            // Broadcast to Web Clients
            webClients.forEach(client => {
                if (client.readyState === WebSocket.OPEN) {
                    client.send(payloadStrOut); 
                    client.send(JSON.stringify({ type: 'espStatus', status: 'online', macAddress: mac }));
                }
            });

            // Process Motor Logs
            const currentMotorStatus = data.payload.motorStatus;
            
            if (currentMotorStatus === 'ON' && lastMotorStatus === 'OFF') {
                motorStartTime = new Date();
                console.log("Motor Started at:", motorStartTime);
            } else if (currentMotorStatus === 'OFF' && lastMotorStatus === 'ON' && motorStartTime) {
                const motorStopTime = new Date();
                const durationMs = motorStopTime - motorStartTime;
                const durationSec = Math.floor(durationMs / 1000);
                
                const durationStr = `${Math.floor(durationSec / 60)}m ${durationSec % 60}s`;

                if (durationSec >= 2) {
                    const optionsDate = { timeZone: 'Asia/Dhaka', day: '2-digit', month: '2-digit', year: 'numeric' };
                    const optionsTime = { timeZone: 'Asia/Dhaka', hour: 'numeric', minute: 'numeric', second: 'numeric', hour12: true };
                    
                    let bdDateFinal;
                    try {
                       const bdDateParts = new Intl.DateTimeFormat('en-GB', optionsDate).formatToParts(motorStopTime);
                       const day = bdDateParts.find(p => p.type === 'day').value;
                       const month = bdDateParts.find(p => p.type === 'month').value;
                       const year = bdDateParts.find(p => p.type === 'year').value;
                       bdDateFinal = `${day}/${month}/${year}`;
                    } catch(e) {
                       bdDateFinal = motorStopTime.toLocaleDateString();
                    }
                    
                    const bdTimeFinal = motorStopTime.toLocaleTimeString('en-US', optionsTime);

                    console.log(`Motor Stopped. Duration: ${durationStr}`);

                    const newLog = new MotorLog({
                        macAddress: mac,
                        startTime: motorStartTime,
                        endTime: motorStopTime,
                        duration: durationStr,
                        bdDate: bdDateFinal,
                        bdTime: bdTimeFinal
                    });
                    newLog.save().then(() => console.log("Log saved to DB")).catch(err => console.error(err));
                }
                motorStartTime = null; 
            }
            lastMotorStatus = currentMotorStatus;
        }
    } catch (err) {
        console.error("MQTT Message Parse Error:", err);
    }
});

mqttClient.on('error', (err) => {
    console.error("MQTT Error:", err);
});


// --- WebSocket Server (For Next.js Dashboard) ---
wss.on('connection', (ws) => {
    console.log('Web Dashboard client connected.');
    webClients.add(ws);
    
    // Send immediate initial status
    ws.send(JSON.stringify({ type: 'espStatus', status: isEspOnline ? 'online' : 'offline' }));

    ws.on('message', async (message) => {
        let data;
        try {
            data = JSON.parse(message);
        } catch (e) {
            return;
        }

        if (data.type === 'command') {
            if (data.command === 'GET_LOG_PAGE') {
                const mac = data.macAddress || DEVICE_MAC;
                const page = data.value || 0;
                const limit = 10;
                
                let query = { macAddress: mac };
                if (data.startDate && data.endDate) {
                    const start = new Date(data.startDate);
                    start.setHours(0,0,0,0);
                    
                    const end = new Date(data.endDate);
                    end.setHours(23,59,59,999);
                    
                    query.startTime = { $gte: start, $lte: end };
                    console.log(`Filtering logs from ${start} to ${end}`);
                }

                try {
                    const totalLogs = await MotorLog.countDocuments(query);
                    const totalPages = Math.ceil(totalLogs / limit);
                    const logs = await MotorLog.find(query)
                        .sort({ createdAt: -1 })
                        .skip(page * limit)
                        .limit(limit);
                    
                    const logStrings = logs.map(log => {
                        const formatBD = (date) => {
                            if (!date) return 'N/A';
                            const optionsDate = { timeZone: 'Asia/Dhaka', day: '2-digit', month: '2-digit', year: 'numeric' };
                            const optionsTime = { timeZone: 'Asia/Dhaka', hour: 'numeric', minute: 'numeric', second: 'numeric', hour12: true };
                            return new Intl.DateTimeFormat('en-GB', optionsDate).format(date) + ' ' + 
                                   new Intl.DateTimeFormat('en-US', optionsTime).format(date);
                        };

                        return JSON.stringify({
                            onTime: formatBD(log.startTime),
                            offTime: log.bdDate + ' ' + log.bdTime,
                            duration: log.duration
                        });
                    });

                    ws.send(JSON.stringify({
                        type: 'logPageUpdate',
                        payload: {
                            motorLogs: logStrings,
                            currentPage: page,
                            totalPages: totalPages
                        }
                    }));
                } catch (err) {
                    console.error("Error fetching logs:", err);
                }
            } else if (data.command === 'CLEAR_LOGS') {
                try {
                    await MotorLog.deleteMany({});
                    console.log("All logs cleared.");
                    webClients.forEach(client => {
                        if (client.readyState === WebSocket.OPEN) {
                             client.send(JSON.stringify({
                                 type: 'statusUpdate',
                                 payload: {
                                    lastAction: "Logs Cleared", 
                                    motorStatus: lastMotorStatus,
                                    systemMode: "Normal"
                                 }
                             }));
                        }
                    });
                } catch (err) {
                    console.error("Error clearing logs:", err);
                }
            } else {
                 // Forward Hardware Commands to ESP32 via MQTT
                 if (mqttClient.connected) {
                     const commandTopic = `device/${DEVICE_MAC}/command`;
                     mqttClient.publish(commandTopic, message.toString(), { qos: 1 });
                     console.log(`Forwarded command to MQTT: ${commandTopic} -> ${message.toString()}`);
                 } else {
                     console.log("MQTT disconnected, cannot send command.");
                 }
            }
        }
    });

    ws.on('close', () => {
        webClients.delete(ws);
        console.log('Web Dashboard client disconnected.');
    });
});

// --- HTTP Static ---
app.use(express.static(path.join(__dirname, '../Dashboard')));
app.get('/status', (req, res) => res.send('server is running'));
app.get('/', (req, res) => res.send('server is running ...'));

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
    console.log(`Server is listening on port ${PORT}`);
    console.log('server is running');
});
