import React, { useState, useEffect, useRef } from 'react';
import {
  StyleSheet,
  Text,
  View,
  ScrollView,
  TouchableOpacity,
  TextInput,
  Modal,
  Alert,
  ActivityIndicator,
  Animated,
  Easing,
  Platform,
  RefreshControl,
  SafeAreaView,
} from 'react-native';
import { StatusBar } from 'expo-status-bar';
import { Ionicons, MaterialCommunityIcons } from '@expo/vector-icons';

// --- API & WebSocket Helpers ---
const getBaseUrl = () => {
  if (Platform.OS === 'web' && typeof window !== 'undefined') {
    return 'http://localhost:3000';
  }
  // Default to localhost, or update to your local network IP (e.g., http://192.168.0.101:3000)
  return 'http://localhost:3000';
};

const getWsUrl = () => {
  if (Platform.OS === 'web' && typeof window !== 'undefined') {
    return 'ws://localhost:3000';
  }
  return 'ws://localhost:3000';
};

export default function App() {
  const [isDarkMode, setIsDarkMode] = useState(true);
  const [currentScreen, setCurrentScreen] = useState('devices'); // 'devices' | 'dashboard'
  const [selectedDevice, setSelectedDevice] = useState(null);

  // --- Devices State ---
  const [devices, setDevices] = useState([]);
  const [loadingDevices, setLoadingDevices] = useState(true);
  const [refreshingDevices, setRefreshingDevices] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  
  // Modals
  const [showAddModal, setShowAddModal] = useState(false);
  const [showEditModal, setShowEditModal] = useState(false);
  const [editDeviceId, setEditDeviceId] = useState(null);
  const [deviceName, setDeviceName] = useState('');
  const [deviceMac, setDeviceMac] = useState('');
  const [isSubmitting, setIsSubmitting] = useState(false);

  // --- Device Dashboard State ---
  const [motorStatus, setMotorStatus] = useState('--');
  const [systemMode, setSystemMode] = useState('--');
  const [lastAction, setLastAction] = useState('--');
  const [wifiSignal, setWifiSignal] = useState(0);
  const [isOnline, setIsOnline] = useState(false);
  const [logs, setLogs] = useState([]);
  const [currentPage, setCurrentPage] = useState(0);
  const [totalPages, setTotalPages] = useState(1);
  const [loadingLogs, setLoadingLogs] = useState(false);

  // --- Authorized Phone Numbers State ---
  const [authorizedNumbers, setAuthorizedNumbers] = useState([]);
  const [newPhone, setNewPhone] = useState('');
  const [loadingNumbers, setLoadingNumbers] = useState(false);
  const [addingNumber, setAddingNumber] = useState(false);

  // --- Popup Confirmation Modal State ---
  const [confirmModal, setConfirmModal] = useState({
    isOpen: false,
    title: '',
    message: '',
    confirmText: 'Confirm',
    isDestructive: false,
    onConfirm: null,
  });

  const openConfirm = ({ title, message, confirmText = 'Confirm', isDestructive = false, onConfirm }) => {
    setConfirmModal({
      isOpen: true,
      title,
      message,
      confirmText,
      isDestructive,
      onConfirm: () => {
        setConfirmModal((prev) => ({ ...prev, isOpen: false }));
        if (onConfirm) onConfirm();
      },
    });
  };

  const closeConfirm = () => {
    setConfirmModal((prev) => ({ ...prev, isOpen: false }));
  };

  // Motor Gear Rotation Animation
  const spinAnim = useRef(new Animated.Value(0)).current;
  const wsRef = useRef(null);

  const colors = isDarkMode
    ? {
        bg: '#0b1120',
        card: '#1e293b',
        cardHover: '#283548',
        border: '#334155',
        text: '#f8fafc',
        subtext: '#94a3b8',
        primary: '#3b82f6',
        success: '#10b981',
        error: '#ef4444',
        accent: '#6366f1',
        inputBg: '#0f172a',
      }
    : {
        bg: '#f1f5f9',
        card: '#ffffff',
        cardHover: '#f8fafc',
        border: '#e2e8f0',
        text: '#0f172a',
        subtext: '#64748b',
        primary: '#2563eb',
        success: '#059669',
        error: '#dc2626',
        accent: '#4f46e5',
        inputBg: '#f8fafc',
      };

  // --- Animation Loop for Motor Running ---
  useEffect(() => {
    let animation = null;
    if (motorStatus === 'ON') {
      spinAnim.setValue(0);
      animation = Animated.loop(
        Animated.timing(spinAnim, {
          toValue: 1,
          duration: 1500,
          easing: Easing.linear,
          useNativeDriver: Platform.OS !== 'web',
        })
      );
      animation.start();
    } else {
      spinAnim.setValue(0);
    }
    return () => {
      if (animation) animation.stop();
    };
  }, [motorStatus]);

  const spin = spinAnim.interpolate({
    inputRange: [0, 1],
    outputRange: ['0deg', '360deg'],
  });

  // --- Fetch Devices ---
  const fetchDevices = async () => {
    try {
      const res = await fetch(`${getBaseUrl()}/api/devices`);
      if (res.ok) {
        const data = await res.json();
        setDevices(data);
      }
    } catch (err) {
      console.log('Error fetching devices:', err.message);
    } finally {
      setLoadingDevices(false);
      setRefreshingDevices(false);
    }
  };

  useEffect(() => {
    fetchDevices();
  }, []);

  const onRefreshDevices = () => {
    setRefreshingDevices(true);
    fetchDevices();
  };

  // --- Add / Edit / Delete Device ---
  const handleAddDevice = async () => {
    const cleanMac = deviceMac.trim().replace(/[:-]/g, '').toUpperCase();
    if (!deviceName.trim() || !cleanMac) {
      Alert.alert('Required', 'Please enter a device name and MAC address.');
      return;
    }
    setIsSubmitting(true);
    try {
      const res = await fetch(`${getBaseUrl()}/api/devices`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          name: deviceName.trim(),
          macAddress: cleanMac,
        }),
      });
      if (res.ok) {
        closeModals();
        fetchDevices();
      } else {
        Alert.alert('Failed', 'Could not add device. Verify MAC address.');
      }
    } catch (err) {
      Alert.alert('Error', 'Failed to connect to backend server.');
    } finally {
      setIsSubmitting(false);
    }
  };

  const handleEditDevice = async () => {
    const cleanMac = deviceMac.trim().replace(/[:-]/g, '').toUpperCase();
    if (!deviceName.trim() || !cleanMac || !editDeviceId) return;

    setIsSubmitting(true);
    try {
      const res = await fetch(`${getBaseUrl()}/api/devices/${editDeviceId}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          name: deviceName.trim(),
          macAddress: cleanMac,
        }),
      });
      if (res.ok) {
        closeModals();
        fetchDevices();
      } else {
        Alert.alert('Failed', 'Could not update device.');
      }
    } catch (err) {
      Alert.alert('Error', 'Failed to connect to backend server.');
    } finally {
      setIsSubmitting(false);
    }
  };

  const handleDeleteDevice = (id, name) => {
    const proceedDelete = async () => {
      try {
        const res = await fetch(`${getBaseUrl()}/api/devices/${id}`, { method: 'DELETE' });
        if (res.ok) {
          setDevices(devices.filter((d) => d.id !== id));
        } else {
          Alert.alert('Failed', 'Could not delete device.');
        }
      } catch (err) {
        Alert.alert('Error', 'Server error deleting device.');
      }
    };

    openConfirm({
      title: 'Delete Device?',
      message: `Are you sure you want to delete "${name}"? All associated settings and activity logs will be removed.`,
      confirmText: 'Delete',
      isDestructive: true,
      onConfirm: proceedDelete,
    });
  };

  const openEditModal = (device) => {
    setEditDeviceId(device.id);
    setDeviceName(device.name);
    setDeviceMac(device.macAddress);
    setShowEditModal(true);
  };

  const closeModals = () => {
    setShowAddModal(false);
    setShowEditModal(false);
    setEditDeviceId(null);
    setDeviceName('');
    setDeviceMac('');
  };

  // --- WebSocket Connection for Dashboard ---
  useEffect(() => {
    if (currentScreen === 'dashboard' && selectedDevice) {
      const mac = selectedDevice.macAddress;
      fetchNumbers(mac);
      const ws = new WebSocket(getWsUrl());
      wsRef.current = ws;

      ws.onopen = () => {
        setIsOnline(true);
        ws.send(JSON.stringify({ type: 'command', command: 'GET_LOG_PAGE', macAddress: mac, value: 0 }));
      };

      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data.trim());
          if (data.type === 'statusUpdate') {
            setIsOnline(true);
            if (data.payload) {
              if (data.payload.motorStatus) setMotorStatus(data.payload.motorStatus);
              if (data.payload.systemMode) setSystemMode(data.payload.systemMode);
              if (data.payload.lastAction) setLastAction(data.payload.lastAction);
              if (data.payload.wifiSignal !== undefined) setWifiSignal(data.payload.wifiSignal);
              if (Array.isArray(data.payload.authorizedNumbers)) {
                setAuthorizedNumbers(data.payload.authorizedNumbers);
              }
            }
          } else if (data.type === 'numbersUpdate' && (!data.macAddress || data.macAddress === mac)) {
            if (Array.isArray(data.authorizedNumbers)) {
              setAuthorizedNumbers(data.authorizedNumbers);
            }
          } else if (data.type === 'logPageUpdate') {
            setIsOnline(true);
            if (data.payload && Array.isArray(data.payload.motorLogs)) {
              const parsed = data.payload.motorLogs.map((item) =>
                typeof item === 'string' ? JSON.parse(item) : item
              );
              setLogs(parsed);
              setCurrentPage(data.payload.currentPage || 0);
              setTotalPages(data.payload.totalPages || 1);
            }
            setLoadingLogs(false);
          } else if (data.type === 'espStatus') {
            setIsOnline(data.status === 'online');
          }
        } catch (e) {}
      };

      ws.onclose = () => {
        setIsOnline(false);
      };

      return () => {
        if (wsRef.current) wsRef.current.close();
      };
    }
  }, [currentScreen, selectedDevice]);

  // --- Fetch Authorized Numbers ---
  const fetchNumbers = async (mac) => {
    if (!mac) return;
    try {
      setLoadingNumbers(true);
      const res = await fetch(`${getBaseUrl()}/api/devices/${mac}/numbers`);
      if (res.ok) {
        const data = await res.json();
        setAuthorizedNumbers(Array.isArray(data) ? data : []);
      }
    } catch (err) {
      console.log('Error fetching numbers:', err);
    } finally {
      setLoadingNumbers(false);
    }
  };

  // --- Add Authorized Number ---
  const handleAddNumber = async () => {
    let num = newPhone.trim();
    if (!num) return;
    if (!num.startsWith('+')) {
      if (num.startsWith('01')) num = '+88' + num;
      else if (num.startsWith('8801')) num = '+' + num;
    }
    if (num.length < 11) {
      if (Platform.OS === 'web') window.alert('Please enter a valid phone number (e.g. 01712345678)');
      else Alert.alert('Invalid Number', 'Please enter a valid phone number (e.g. 01712345678)');
      return;
    }
    try {
      setAddingNumber(true);
      const res = await fetch(`${getBaseUrl()}/api/devices/${selectedDevice.macAddress}/numbers`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ number: num }),
      });
      if (res.ok) {
        const updated = await res.json();
        setAuthorizedNumbers(updated);
        setNewPhone('');
      }
    } catch (err) {
      console.log('Error adding number:', err);
    } finally {
      setAddingNumber(false);
    }
  };

  // --- Delete Authorized Number ---
  const handleDeleteNumber = (num) => {
    const confirmAction = () => {
      fetch(`${getBaseUrl()}/api/devices/${selectedDevice.macAddress}/numbers/${encodeURIComponent(num)}`, {
        method: 'DELETE',
      })
        .then((res) => res.json())
        .then((updated) => setAuthorizedNumbers(updated))
        .catch((err) => console.log('Error deleting number:', err));
    };

    openConfirm({
      title: 'Remove Number?',
      message: `Are you sure you want to remove ${num} from the authorized whitelist?`,
      confirmText: 'Remove',
      isDestructive: true,
      onConfirm: confirmAction,
    });
  };

  const sendWsCommand = (cmd) => {
    if (!wsRef.current || wsRef.current.readyState !== WebSocket.OPEN) {
      Alert.alert('Offline', 'Server is not connected.');
      return;
    }
    const execute = () => {
      wsRef.current.send(
        JSON.stringify({
          type: 'command',
          command: cmd,
          macAddress: selectedDevice?.macAddress,
        })
      );
    };

    if (cmd === 'RELAY_1') {
      openConfirm({
        title: 'Start Motor Pump?',
        message: 'Are you sure you want to turn ON the motor pump now?',
        confirmText: 'START',
        isDestructive: false,
        onConfirm: execute,
      });
    } else if (cmd === 'RELAY_2') {
      openConfirm({
        title: 'Stop Motor Pump?',
        message: 'Are you sure you want to turn OFF the motor pump now?',
        confirmText: 'STOP',
        isDestructive: true,
        onConfirm: execute,
      });
    } else if (cmd === 'CLEAR_LOGS') {
      openConfirm({
        title: 'Clear All Activity Logs?',
        message: 'Are you sure you want to permanently delete all activity logs for this device?',
        confirmText: 'Clear Logs',
        isDestructive: true,
        onConfirm: execute,
      });
    } else if (cmd === 'RESTART_ESP') {
      openConfirm({
        title: 'Reboot ESP32?',
        message: 'Are you sure you want to remotely restart the ESP32?',
        confirmText: 'Reboot',
        isDestructive: true,
        onConfirm: execute,
      });
    } else {
      execute();
    }
  };

  const requestLogPage = (page) => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      setLoadingLogs(true);
      wsRef.current.send(
        JSON.stringify({
          type: 'command',
          command: 'GET_LOG_PAGE',
          macAddress: selectedDevice?.macAddress,
          value: page,
        })
      );
    }
  };

  const filteredDevices = devices.filter(
    (d) =>
      (d.name && d.name.toLowerCase().includes(searchQuery.toLowerCase())) ||
      (d.macAddress && d.macAddress.toLowerCase().includes(searchQuery.toLowerCase()))
  );

  // ==========================================
  // RENDER: DEVICE LIST SCREEN
  // ==========================================
  const renderDevicesScreen = () => (
    <ScrollView
      style={{ flex: 1 }}
      contentContainerStyle={{ padding: 16, paddingBottom: 40 }}
      refreshControl={
        <RefreshControl refreshing={refreshingDevices} onRefresh={onRefreshDevices} tintColor={colors.primary} />
      }
    >
      {/* Header Info */}
      <View style={{ marginBottom: 16 }}>
        <Text style={[styles.title, { color: colors.text }]}>My Devices</Text>
        <Text style={[styles.subtitle, { color: colors.subtext }]}>
          Control and monitor all registered Mutho-Sech pump controllers.
        </Text>
      </View>

      {/* Search & Add Bar */}
      <View style={styles.searchBarContainer}>
        <View style={[styles.searchInputWrapper, { backgroundColor: colors.card, borderColor: colors.border }]}>
          <Ionicons name="search" size={18} color={colors.subtext} style={{ marginRight: 8 }} />
          <TextInput
            style={[styles.searchInput, { color: colors.text }]}
            placeholder="Search device name or MAC..."
            placeholderTextColor={colors.subtext}
            value={searchQuery}
            onChangeText={setSearchQuery}
          />
        </View>

        <TouchableOpacity
          style={[styles.addButton, { backgroundColor: colors.primary }]}
          onPress={() => setShowAddModal(true)}
        >
          <Ionicons name="add" size={24} color="#ffffff" />
        </TouchableOpacity>
      </View>

      {/* Devices List */}
      {loadingDevices ? (
        <View style={{ paddingVertical: 60, alignItems: 'center' }}>
          <ActivityIndicator size="large" color={colors.primary} />
          <Text style={{ color: colors.subtext, marginTop: 12, fontSize: 14 }}>Loading devices...</Text>
        </View>
      ) : filteredDevices.length === 0 ? (
        <View style={[styles.emptyCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
          <MaterialCommunityIcons name="water-pump-off" size={54} color={colors.subtext} />
          <Text style={[styles.emptyTitle, { color: colors.text }]}>No devices found</Text>
          <Text style={[styles.emptySubtitle, { color: colors.subtext }]}>
            {searchQuery ? 'Try a different search term.' : 'Tap the "+" button above to add your first smart pump.'}
          </Text>
        </View>
      ) : (
        filteredDevices.map((device) => {
          const isDevOnline = device.network?.isOnline;
          const isRunning = device.currentStatus?.powerState === 'ON';
          const signal = device.network?.signalStrength || 0;

          return (
            <View
              key={device.id}
              style={[
                styles.deviceCard,
                { backgroundColor: colors.card, borderColor: colors.border },
              ]}
            >
              {/* Card Header */}
              <View style={styles.cardHeader}>
                <View style={styles.badgeRow}>
                  <View
                    style={[
                      styles.statusPill,
                      {
                        backgroundColor: isDevOnline ? 'rgba(16, 185, 129, 0.15)' : 'rgba(148, 163, 184, 0.15)',
                      },
                    ]}
                  >
                    <View
                      style={[
                        styles.statusDot,
                        { backgroundColor: isDevOnline ? colors.success : colors.subtext },
                      ]}
                    />
                    <Text
                      style={[
                        styles.statusText,
                        { color: isDevOnline ? colors.success : colors.subtext },
                      ]}
                    >
                      {isDevOnline ? 'Online' : 'Offline'}
                    </Text>
                  </View>

                  <View
                    style={[
                      styles.statusPill,
                      {
                        backgroundColor: isRunning ? 'rgba(16, 185, 129, 0.2)' : 'rgba(148, 163, 184, 0.15)',
                      },
                    ]}
                  >
                    <Text
                      style={[
                        styles.statusText,
                        { color: isRunning ? colors.success : colors.subtext, fontWeight: '700' },
                      ]}
                    >
                      {isRunning ? 'RUNNING' : 'STOPPED'}
                    </Text>
                  </View>
                </View>

                {/* Edit / Delete Icons */}
                <View style={{ flexDirection: 'row', gap: 6 }}>
                  <TouchableOpacity
                    style={[styles.iconButton, { borderColor: colors.border }]}
                    onPress={() => openEditModal(device)}
                  >
                    <Ionicons name="pencil" size={16} color={colors.subtext} />
                  </TouchableOpacity>
                  <TouchableOpacity
                    style={[styles.iconButton, { borderColor: colors.border }]}
                    onPress={() => handleDeleteDevice(device.id, device.name)}
                  >
                    <Ionicons name="trash-outline" size={16} color={colors.error} />
                  </TouchableOpacity>
                </View>
              </View>

              {/* Device Info */}
              <Text style={[styles.deviceName, { color: colors.text }]}>{device.name}</Text>
              <Text style={[styles.deviceMac, { color: colors.subtext }]}>MAC: {device.macAddress}</Text>

              {/* Quick Details Bar */}
              <View style={[styles.quickDetails, { backgroundColor: colors.bg, borderColor: colors.border }]}>
                <View>
                  <Text style={[styles.detailLabel, { color: colors.subtext }]}>Mode</Text>
                  <Text style={[styles.detailValue, { color: colors.text }]}>
                    {device.currentStatus?.currentMode || 'Manual'}
                  </Text>
                </View>
                <View style={{ alignItems: 'flex-end' }}>
                  <Text style={[styles.detailLabel, { color: colors.subtext }]}>Signal</Text>
                  <Text style={[styles.detailValue, { color: colors.text }]}>
                    <Ionicons name="wifi" size={12} color={colors.primary} /> {signal}%
                  </Text>
                </View>
              </View>

              {/* Open Dashboard Button */}
              <TouchableOpacity
                style={[styles.openButton, { backgroundColor: colors.primary }]}
                onPress={() => {
                  setSelectedDevice(device);
                  setCurrentScreen('dashboard');
                }}
              >
                <Ionicons name="speedometer-outline" size={18} color="#fff" style={{ marginRight: 6 }} />
                <Text style={styles.openButtonText}>Open Dashboard</Text>
              </TouchableOpacity>
            </View>
          );
        })
      )}
    </ScrollView>
  );

  // ==========================================
  // RENDER: DEVICE DASHBOARD SCREEN
  // ==========================================
  const renderDashboardScreen = () => {
    const isMotorOn = motorStatus === 'ON';

    return (
      <ScrollView style={{ flex: 1 }} contentContainerStyle={{ padding: 16, paddingBottom: 40 }}>
        {/* Top Navigation */}
        <View style={styles.dashboardTopNav}>
          <TouchableOpacity
            style={[styles.backButton, { backgroundColor: colors.card, borderColor: colors.border }]}
            onPress={() => {
              if (wsRef.current) wsRef.current.close();
              setCurrentScreen('devices');
            }}
          >
            <Ionicons name="arrow-back" size={20} color={colors.text} />
          </TouchableOpacity>

          <View style={{ flex: 1, marginHorizontal: 12 }}>
            <Text style={[styles.dashDeviceName, { color: colors.text }]} numberOfLines={1}>
              {selectedDevice?.name || 'Device Dashboard'}
            </Text>
            <Text style={[styles.dashDeviceMac, { color: colors.subtext }]}>
              MAC: {selectedDevice?.macAddress}
            </Text>
          </View>

          <View
            style={[
              styles.statusPill,
              { backgroundColor: isOnline ? 'rgba(16, 185, 129, 0.15)' : 'rgba(239, 68, 68, 0.15)' },
            ]}
          >
            <View
              style={[styles.statusDot, { backgroundColor: isOnline ? colors.success : colors.error }]}
            />
            <Text style={{ fontSize: 12, fontWeight: '700', color: isOnline ? colors.success : colors.error }}>
              {isOnline ? 'Online' : 'Offline'}
            </Text>
          </View>
        </View>

        {/* Hero Control Card */}
        <View style={[styles.heroCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
          <Text style={[styles.heroCardSubtitle, { color: colors.subtext }]}>PUMP MOTOR CONTROL</Text>

          {/* Animated Motor Gear */}
          <View style={{ alignItems: 'center', marginVertical: 20 }}>
            <Animated.View
              style={{
                transform: [{ rotate: spin }],
                width: 100,
                height: 100,
                alignItems: 'center',
                justifyContent: 'center',
                backgroundColor: isMotorOn ? 'rgba(16, 185, 129, 0.15)' : 'rgba(148, 163, 184, 0.1)',
                borderRadius: 50,
                borderWidth: 2,
                borderColor: isMotorOn ? colors.success : colors.border,
              }}
            >
              <MaterialCommunityIcons
                name="cog-clockwise"
                size={64}
                color={isMotorOn ? colors.success : colors.subtext}
              />
            </Animated.View>

            <Text style={[styles.statusValue, { color: isMotorOn ? colors.success : colors.subtext }]}>
              {motorStatus}
            </Text>
            <Text style={{ fontSize: 13, color: colors.subtext, marginTop: 4 }}>
              {isMotorOn ? 'Motor pump is actively running' : 'Motor pump is stopped'}
            </Text>
          </View>

          {/* Big Action Buttons */}
          <View style={styles.actionButtonRow}>
            <TouchableOpacity
              style={[
                styles.controlBtn,
                {
                  backgroundColor: colors.success,
                  opacity: isOnline ? 1 : 0.5,
                },
              ]}
              disabled={!isOnline}
              onPress={() => sendWsCommand('RELAY_1')}
            >
              <Ionicons name="power" size={22} color="#fff" style={{ marginRight: 6 }} />
              <Text style={styles.controlBtnText}>START</Text>
            </TouchableOpacity>

            <TouchableOpacity
              style={[
                styles.controlBtn,
                {
                  backgroundColor: colors.error,
                  opacity: isOnline ? 1 : 0.5,
                },
              ]}
              disabled={!isOnline}
              onPress={() => sendWsCommand('RELAY_2')}
            >
              <Ionicons name="stop" size={22} color="#fff" style={{ marginRight: 6 }} />
              <Text style={styles.controlBtnText}>STOP</Text>
            </TouchableOpacity>
          </View>
        </View>

        {/* Metrics Grid */}
        <View style={styles.metricsGrid}>
          <View style={[styles.metricCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
            <Text style={[styles.metricLabel, { color: colors.subtext }]}>SYSTEM MODE</Text>
            <Text style={[styles.metricVal, { color: colors.primary }]}>{systemMode}</Text>
          </View>

          <View style={[styles.metricCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
            <View style={{ flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' }}>
              <Text style={[styles.metricLabel, { color: colors.subtext }]}>LAST ACTION</Text>
              <TouchableOpacity onPress={() => sendWsCommand('RESTART_ESP')} disabled={!isOnline}>
                <Ionicons name="refresh" size={14} color={colors.error} />
              </TouchableOpacity>
            </View>
            <Text style={[styles.metricVal, { color: colors.text }]} numberOfLines={1}>
              {lastAction}
            </Text>
          </View>

          <View style={[styles.metricCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
            <Text style={[styles.metricLabel, { color: colors.subtext }]}>WIFI SIGNAL</Text>
            <Text style={[styles.metricVal, { color: colors.text }]}>{wifiSignal}%</Text>
          </View>
        </View>

        {/* Authorized Phone Numbers (Call/SMS Whitelist) */}
        <View style={[styles.numbersCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
          <View style={styles.numbersHeader}>
            <View style={{ flexDirection: 'row', alignItems: 'center' }}>
              <Ionicons name="call" size={18} color={colors.primary} style={{ marginRight: 8 }} />
              <View>
                <Text style={[styles.logsTitle, { color: colors.text }]}>Authorized Numbers</Text>
                <Text style={{ fontSize: 11, color: colors.subtext }}>IVR Call & SMS Whitelist</Text>
              </View>
            </View>
            <View style={[styles.countBadge, { backgroundColor: 'rgba(59, 130, 246, 0.15)' }]}>
              <Text style={{ fontSize: 11, fontWeight: '700', color: colors.primary }}>
                {authorizedNumbers.length} Registered
              </Text>
            </View>
          </View>

          {/* Add Number Input Row */}
          <View style={styles.numberInputRow}>
            <TextInput
              style={[
                styles.numberInput,
                { backgroundColor: colors.inputBg, borderColor: colors.border, color: colors.text },
              ]}
              placeholder="e.g. 01712345678 or +8801..."
              placeholderTextColor={colors.subtext}
              value={newPhone}
              onChangeText={setNewPhone}
            />
            <TouchableOpacity
              style={[styles.numberAddBtn, { backgroundColor: colors.primary }]}
              onPress={handleAddNumber}
              disabled={addingNumber || !newPhone.trim()}
            >
              {addingNumber ? (
                <ActivityIndicator size="small" color="#fff" />
              ) : (
                <>
                  <Ionicons name="add" size={18} color="#fff" />
                  <Text style={{ color: '#fff', fontSize: 12, fontWeight: '700' }}>Add</Text>
                </>
              )}
            </TouchableOpacity>
          </View>

          {/* Numbers List */}
          {loadingNumbers && authorizedNumbers.length === 0 ? (
            <ActivityIndicator size="small" color={colors.primary} style={{ marginVertical: 16 }} />
          ) : authorizedNumbers.length === 0 ? (
            <Text style={{ textAlign: 'center', color: colors.subtext, marginVertical: 14, fontSize: 12 }}>
              No phone numbers registered yet. Add one above.
            </Text>
          ) : (
            authorizedNumbers.map((num, idx) => (
              <View
                key={idx}
                style={[
                  styles.numberItem,
                  { borderBottomColor: colors.border, borderBottomWidth: idx === authorizedNumbers.length - 1 ? 0 : 1 },
                ]}
              >
                <View style={{ flexDirection: 'row', alignItems: 'center' }}>
                  <View
                    style={{
                      width: 32,
                      height: 32,
                      borderRadius: 16,
                      backgroundColor: 'rgba(16, 185, 129, 0.12)',
                      alignItems: 'center',
                      justifyContent: 'center',
                      marginRight: 10,
                    }}
                  >
                    <Ionicons name="shield-checkmark" size={16} color={colors.success} />
                  </View>
                  <View>
                    <Text style={[styles.numberText, { color: colors.text }]}>{num}</Text>
                    <Text style={{ fontSize: 10, color: colors.subtext }}>Call (DTMF) & SMS</Text>
                  </View>
                </View>

                <TouchableOpacity
                  onPress={() => handleDeleteNumber(num)}
                  style={{ padding: 6 }}
                >
                  <Ionicons name="trash-outline" size={18} color={colors.error} />
                </TouchableOpacity>
              </View>
            ))
          )}
        </View>

        {/* Activity Logs Card */}
        <View style={[styles.logsCard, { backgroundColor: colors.card, borderColor: colors.border }]}>
          <View style={styles.logsHeader}>
            <View>
              <Text style={[styles.logsTitle, { color: colors.text }]}>Motor Activity Logs</Text>
              <Text style={{ fontSize: 12, color: colors.subtext }}>Duration & timestamps</Text>
            </View>
            <TouchableOpacity
              style={[styles.clearBtn, { borderColor: colors.error }]}
              onPress={() => sendWsCommand('CLEAR_LOGS')}
              disabled={!isOnline}
            >
              <Text style={{ fontSize: 11, color: colors.error, fontWeight: '700' }}>Clear Logs</Text>
            </TouchableOpacity>
          </View>

          {loadingLogs ? (
            <ActivityIndicator size="small" color={colors.primary} style={{ marginVertical: 30 }} />
          ) : logs.length === 0 ? (
            <Text style={{ textAlign: 'center', color: colors.subtext, marginVertical: 24, fontSize: 13 }}>
              No recent activity logs available.
            </Text>
          ) : (
            logs.map((log, index) => (
              <View
                key={index}
                style={[
                  styles.logItem,
                  { borderBottomColor: colors.border, borderBottomWidth: index === logs.length - 1 ? 0 : 1 },
                ]}
              >
                <View style={{ flex: 1 }}>
                  <Text style={[styles.logTime, { color: colors.text }]}>
                    ON: {log.onTime || log.bdTime || 'N/A'}
                  </Text>
                  <Text style={[styles.logTime, { color: colors.subtext, marginTop: 2 }]}>
                    OFF: {log.offTime || log.bdTime || 'N/A'}
                  </Text>
                </View>
                <View style={[styles.durationBadge, { backgroundColor: colors.bg }]}>
                  <Text style={[styles.durationText, { color: colors.primary }]}>{log.duration || 'N/A'}</Text>
                </View>
              </View>
            ))
          )}

          {/* Pagination */}
          <View style={styles.paginationRow}>
            <TouchableOpacity
              style={[styles.pageBtn, { borderColor: colors.border, opacity: currentPage === 0 ? 0.3 : 1 }]}
              disabled={currentPage === 0 || loadingLogs}
              onPress={() => requestLogPage(currentPage - 1)}
            >
              <Ionicons name="chevron-back" size={16} color={colors.text} />
            </TouchableOpacity>

            <Text style={{ fontSize: 12, color: colors.subtext, fontWeight: '600' }}>
              Page {currentPage + 1} of {Math.max(1, totalPages)}
            </Text>

            <TouchableOpacity
              style={[
                styles.pageBtn,
                { borderColor: colors.border, opacity: currentPage >= totalPages - 1 ? 0.3 : 1 },
              ]}
              disabled={currentPage >= totalPages - 1 || loadingLogs}
              onPress={() => requestLogPage(currentPage + 1)}
            >
              <Ionicons name="chevron-forward" size={16} color={colors.text} />
            </TouchableOpacity>
          </View>
        </View>
      </ScrollView>
    );
  };

  return (
    <SafeAreaView style={[styles.safeArea, { backgroundColor: colors.bg }]}>
      <StatusBar style={isDarkMode ? 'light' : 'dark'} />

      {/* Global Navbar */}
      <View style={[styles.navbar, { backgroundColor: colors.card, borderBottomColor: colors.border }]}>
        <View style={{ flexDirection: 'row', alignItems: 'center' }}>
          <View style={[styles.logoIcon, { backgroundColor: colors.primary }]}>
            <Ionicons name="water" size={18} color="#fff" />
          </View>
          <Text style={[styles.logoText, { color: colors.text }]}>
            Mutho<Text style={{ color: colors.primary }}>-Sech</Text>
          </Text>
        </View>

        <TouchableOpacity
          style={[styles.themeToggle, { borderColor: colors.border }]}
          onPress={() => setIsDarkMode(!isDarkMode)}
        >
          <Ionicons name={isDarkMode ? 'sunny' : 'moon'} size={18} color={isDarkMode ? '#fbbf24' : '#64748b'} />
        </TouchableOpacity>
      </View>

      {/* Screen Router */}
      {currentScreen === 'devices' ? renderDevicesScreen() : renderDashboardScreen()}

      {/* Add Device Modal */}
      <Modal visible={showAddModal} transparent animationType="fade">
        <View style={styles.modalOverlay}>
          <View style={[styles.modalBox, { backgroundColor: colors.card, borderColor: colors.border }]}>
            <Text style={[styles.modalTitle, { color: colors.text }]}>Add New Device</Text>
            <Text style={[styles.modalSubtitle, { color: colors.subtext }]}>
              Enter device label and ESP32 MAC address.
            </Text>

            <TextInput
              style={[styles.modalInput, { backgroundColor: colors.inputBg, color: colors.text, borderColor: colors.border }]}
              placeholder="Device Name (e.g., Pump 1)"
              placeholderTextColor={colors.subtext}
              value={deviceName}
              onChangeText={setDeviceName}
            />

            <TextInput
              style={[styles.modalInput, { backgroundColor: colors.inputBg, color: colors.text, borderColor: colors.border, textTransform: 'uppercase' }]}
              placeholder="MAC Address (e.g., 3C8A1F9AC404)"
              placeholderTextColor={colors.subtext}
              value={deviceMac}
              onChangeText={setDeviceMac}
              autoCapitalize="characters"
            />

            <View style={styles.modalBtnRow}>
              <TouchableOpacity style={[styles.modalBtn, { borderColor: colors.border, borderWidth: 1 }]} onPress={closeModals}>
                <Text style={{ color: colors.text, fontWeight: '600' }}>Cancel</Text>
              </TouchableOpacity>
              <TouchableOpacity
                style={[styles.modalBtn, { backgroundColor: colors.primary }]}
                onPress={handleAddDevice}
                disabled={isSubmitting}
              >
                {isSubmitting ? (
                  <ActivityIndicator size="small" color="#fff" />
                ) : (
                  <Text style={{ color: '#fff', fontWeight: '700' }}>Save Device</Text>
                )}
              </TouchableOpacity>
            </View>
          </View>
        </View>
      </Modal>

      {/* Edit Device Modal */}
      <Modal visible={showEditModal} transparent animationType="fade">
        <View style={styles.modalOverlay}>
          <View style={[styles.modalBox, { backgroundColor: colors.card, borderColor: colors.border }]}>
            <Text style={[styles.modalTitle, { color: colors.text }]}>Edit Device</Text>
            <Text style={[styles.modalSubtitle, { color: colors.subtext }]}>Update name or MAC address.</Text>

            <TextInput
              style={[styles.modalInput, { backgroundColor: colors.inputBg, color: colors.text, borderColor: colors.border }]}
              placeholder="Device Name"
              placeholderTextColor={colors.subtext}
              value={deviceName}
              onChangeText={setDeviceName}
            />

            <TextInput
              style={[styles.modalInput, { backgroundColor: colors.inputBg, color: colors.text, borderColor: colors.border, textTransform: 'uppercase' }]}
              placeholder="MAC Address"
              placeholderTextColor={colors.subtext}
              value={deviceMac}
              onChangeText={setDeviceMac}
              autoCapitalize="characters"
            />

            <View style={styles.modalBtnRow}>
              <TouchableOpacity style={[styles.modalBtn, { borderColor: colors.border, borderWidth: 1 }]} onPress={closeModals}>
                <Text style={{ color: colors.text, fontWeight: '600' }}>Cancel</Text>
              </TouchableOpacity>
              <TouchableOpacity
                style={[styles.modalBtn, { backgroundColor: colors.primary }]}
                onPress={handleEditDevice}
                disabled={isSubmitting}
              >
                {isSubmitting ? (
                  <ActivityIndicator size="small" color="#fff" />
                ) : (
                  <Text style={{ color: '#fff', fontWeight: '700' }}>Save Changes</Text>
                )}
              </TouchableOpacity>
            </View>
          </View>
        </View>
      </Modal>

      {/* Custom Popup Confirmation Modal */}
      <Modal visible={confirmModal.isOpen} transparent animationType="fade">
        <View style={styles.modalOverlay}>
          <View style={[styles.modalBox, { backgroundColor: colors.card, borderColor: colors.border }]}>
            <View style={{ flexDirection: 'row', alignItems: 'center', marginBottom: 8 }}>
              <Ionicons
                name={confirmModal.isDestructive ? 'alert-circle' : 'checkmark-circle'}
                size={24}
                color={confirmModal.isDestructive ? colors.error : colors.success}
                style={{ marginRight: 8 }}
              />
              <Text style={[styles.modalTitle, { color: colors.text }]}>{confirmModal.title}</Text>
            </View>
            <Text style={[styles.modalSubtitle, { color: colors.subtext, marginTop: 4, marginBottom: 20, lineHeight: 18 }]}>
              {confirmModal.message}
            </Text>
            <View style={styles.modalBtnRow}>
              <TouchableOpacity
                style={[styles.modalBtn, { borderWidth: 1, borderColor: colors.border }]}
                onPress={closeConfirm}
              >
                <Text style={{ color: colors.text, fontWeight: '600' }}>Cancel</Text>
              </TouchableOpacity>
              <TouchableOpacity
                style={[
                  styles.modalBtn,
                  { backgroundColor: confirmModal.isDestructive ? colors.error : colors.success },
                ]}
                onPress={confirmModal.onConfirm}
              >
                <Text style={{ color: '#fff', fontWeight: '800' }}>{confirmModal.confirmText}</Text>
              </TouchableOpacity>
            </View>
          </View>
        </View>
      </Modal>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  safeArea: {
    flex: 1,
  },
  navbar: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    paddingHorizontal: 16,
    paddingVertical: 12,
    borderBottomWidth: 1,
  },
  logoIcon: {
    width: 32,
    height: 32,
    borderRadius: 8,
    alignItems: 'center',
    justifyContent: 'center',
    marginRight: 8,
  },
  logoText: {
    fontSize: 18,
    fontWeight: '800',
    letterSpacing: -0.5,
  },
  themeToggle: {
    width: 36,
    height: 36,
    borderRadius: 18,
    borderWidth: 1,
    alignItems: 'center',
    justifyContent: 'center',
  },
  title: {
    fontSize: 24,
    fontWeight: '800',
    letterSpacing: -0.5,
  },
  subtitle: {
    fontSize: 13,
    marginTop: 2,
  },
  searchBarContainer: {
    flexDirection: 'row',
    alignItems: 'center',
    marginBottom: 16,
    gap: 8,
  },
  searchInputWrapper: {
    flex: 1,
    flexDirection: 'row',
    alignItems: 'center',
    borderWidth: 1,
    borderRadius: 12,
    paddingHorizontal: 12,
    height: 44,
  },
  searchInput: {
    flex: 1,
    fontSize: 14,
  },
  addButton: {
    width: 44,
    height: 44,
    borderRadius: 12,
    alignItems: 'center',
    justifyContent: 'center',
  },
  emptyCard: {
    padding: 32,
    borderRadius: 16,
    alignItems: 'center',
    justifyContent: 'center',
    borderWidth: 1,
    borderStyle: 'dashed',
    marginTop: 20,
  },
  emptyTitle: {
    fontSize: 17,
    fontWeight: '700',
    marginTop: 12,
  },
  emptySubtitle: {
    fontSize: 13,
    textAlign: 'center',
    marginTop: 4,
    lineHeight: 18,
  },
  deviceCard: {
    padding: 16,
    borderRadius: 16,
    borderWidth: 1,
    marginBottom: 12,
    shadowColor: '#000',
    shadowOpacity: 0.05,
    shadowOffset: { width: 0, height: 2 },
    shadowRadius: 8,
    elevation: 2,
  },
  cardHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    marginBottom: 8,
  },
  badgeRow: {
    flexDirection: 'row',
    gap: 6,
  },
  statusPill: {
    flexDirection: 'row',
    alignItems: 'center',
    paddingHorizontal: 8,
    paddingVertical: 4,
    borderRadius: 8,
  },
  statusDot: {
    width: 6,
    height: 6,
    borderRadius: 3,
    marginRight: 6,
  },
  statusText: {
    fontSize: 11,
    fontWeight: '600',
  },
  iconButton: {
    width: 32,
    height: 32,
    borderRadius: 8,
    borderWidth: 1,
    alignItems: 'center',
    justifyContent: 'center',
  },
  deviceName: {
    fontSize: 17,
    fontWeight: '700',
  },
  deviceMac: {
    fontSize: 12,
    fontFamily: Platform.OS === 'ios' ? 'Courier' : 'monospace',
    marginTop: 2,
  },
  quickDetails: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    padding: 10,
    borderRadius: 10,
    borderWidth: 1,
    marginVertical: 12,
  },
  detailLabel: {
    fontSize: 11,
    fontWeight: '500',
  },
  detailValue: {
    fontSize: 13,
    fontWeight: '700',
    marginTop: 2,
  },
  openButton: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    paddingVertical: 10,
    borderRadius: 10,
  },
  openButtonText: {
    color: '#fff',
    fontSize: 14,
    fontWeight: '700',
  },
  dashboardTopNav: {
    flexDirection: 'row',
    alignItems: 'center',
    marginBottom: 16,
  },
  backButton: {
    width: 40,
    height: 40,
    borderRadius: 12,
    borderWidth: 1,
    alignItems: 'center',
    justifyContent: 'center',
  },
  dashDeviceName: {
    fontSize: 18,
    fontWeight: '800',
  },
  dashDeviceMac: {
    fontSize: 11,
    fontFamily: Platform.OS === 'ios' ? 'Courier' : 'monospace',
  },
  heroCard: {
    padding: 20,
    borderRadius: 20,
    borderWidth: 1,
    alignItems: 'center',
    marginBottom: 12,
  },
  heroCardSubtitle: {
    fontSize: 11,
    fontWeight: '800',
    letterSpacing: 1,
  },
  statusValue: {
    fontSize: 32,
    fontWeight: '900',
    marginTop: 12,
  },
  actionButtonRow: {
    flexDirection: 'row',
    width: '100%',
    gap: 12,
    marginTop: 12,
  },
  controlBtn: {
    flex: 1,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    paddingVertical: 14,
    borderRadius: 14,
  },
  controlBtnText: {
    color: '#fff',
    fontSize: 16,
    fontWeight: '800',
  },
  metricsGrid: {
    flexDirection: 'row',
    gap: 8,
    marginBottom: 12,
  },
  metricCard: {
    flex: 1,
    padding: 12,
    borderRadius: 14,
    borderWidth: 1,
  },
  metricLabel: {
    fontSize: 10,
    fontWeight: '700',
    letterSpacing: 0.5,
  },
  metricVal: {
    fontSize: 14,
    fontWeight: '800',
    marginTop: 4,
  },
  numbersCard: {
    padding: 16,
    borderRadius: 18,
    borderWidth: 1,
    marginBottom: 16,
  },
  numbersHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    marginBottom: 12,
  },
  countBadge: {
    paddingHorizontal: 8,
    paddingVertical: 4,
    borderRadius: 8,
  },
  numberInputRow: {
    flexDirection: 'row',
    gap: 8,
    marginBottom: 12,
  },
  numberInput: {
    flex: 1,
    borderWidth: 1,
    borderRadius: 10,
    paddingHorizontal: 12,
    height: 40,
    fontSize: 13,
  },
  numberAddBtn: {
    paddingHorizontal: 14,
    height: 40,
    borderRadius: 10,
    alignItems: 'center',
    justifyContent: 'center',
    flexDirection: 'row',
    gap: 4,
  },
  numberItem: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    paddingVertical: 10,
  },
  numberText: {
    fontSize: 13,
    fontWeight: '700',
    fontFamily: Platform.OS === 'ios' ? 'Courier' : 'monospace',
  },
  logsCard: {
    padding: 16,
    borderRadius: 18,
    borderWidth: 1,
  },
  logsHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    marginBottom: 12,
  },
  logsTitle: {
    fontSize: 16,
    fontWeight: '800',
  },
  clearBtn: {
    paddingHorizontal: 8,
    paddingVertical: 4,
    borderRadius: 6,
    borderWidth: 1,
  },
  logItem: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    paddingVertical: 10,
  },
  logTime: {
    fontSize: 12,
    fontWeight: '500',
  },
  durationBadge: {
    paddingHorizontal: 8,
    paddingVertical: 4,
    borderRadius: 6,
  },
  durationText: {
    fontSize: 12,
    fontWeight: '700',
    fontFamily: Platform.OS === 'ios' ? 'Courier' : 'monospace',
  },
  paginationRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    marginTop: 12,
    paddingTop: 8,
  },
  pageBtn: {
    width: 32,
    height: 32,
    borderRadius: 8,
    borderWidth: 1,
    alignItems: 'center',
    justifyContent: 'center',
  },
  modalOverlay: {
    flex: 1,
    backgroundColor: 'rgba(0,0,0,0.6)',
    alignItems: 'center',
    justifyContent: 'center',
    padding: 20,
  },
  modalBox: {
    width: '100%',
    maxWidth: 400,
    borderRadius: 18,
    borderWidth: 1,
    padding: 20,
  },
  modalTitle: {
    fontSize: 19,
    fontWeight: '800',
  },
  modalSubtitle: {
    fontSize: 12,
    marginTop: 2,
    marginBottom: 16,
  },
  modalInput: {
    borderWidth: 1,
    borderRadius: 10,
    paddingHorizontal: 12,
    height: 42,
    fontSize: 14,
    marginBottom: 12,
  },
  modalBtnRow: {
    flexDirection: 'row',
    justifyContent: 'flex-end',
    gap: 8,
    marginTop: 8,
  },
  modalBtn: {
    paddingHorizontal: 16,
    paddingVertical: 10,
    borderRadius: 10,
  },
});
