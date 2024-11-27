// Globalne zmienne
let currentPage = 'pumps';
let pumpsData = [];

// Inicjalizacja przy załadowaniu strony
document.addEventListener('DOMContentLoaded', () => {
    // Obsługa nawigacji
    document.querySelectorAll('.nav-btn').forEach(btn => {
        btn.addEventListener('click', () => switchPage(btn.dataset.page));
    });

    // Pobierz początkowe dane
    loadPumpsData();
    loadMQTTConfig();
    updateSystemInfo();

    // Odświeżaj dane co 5 sekund
    setInterval(() => {
        loadPumpsData();
        updateSystemInfo();
    }, 5000);
});

// Przełączanie stron
function switchPage(pageName) {
    document.querySelectorAll('.page').forEach(page => {
        page.style.display = 'none';
    });
    document.getElementById(`${pageName}-page`).style.display = 'block';

    document.querySelectorAll('.nav-btn').forEach(btn => {
        btn.classList.remove('active');
        if (btn.dataset.page === pageName) {
            btn.classList.add('active');
        }
    });

    currentPage = pageName;
}

// Ładowanie danych pomp
async function loadPumpsData() {
    try {
        const response = await fetch('/api/pumps');
        const data = await response.json();
        pumpsData = data.pumps;
        renderPumps();
    } catch (error) {
        console.error('Błąd pobierania danych pomp:', error);
    }
}

// Renderowanie kart pomp
function renderPumps() {
    const container = document.querySelector('.pumps-grid');
    container.innerHTML = '';

    pumpsData.forEach((pump, index) => {
        const card = document.createElement('div');
        card.className = 'pump-card';
        card.innerHTML = `
            <div class="pump-header">
                <h3>Pompa ${index + 1}</h3>
                <label class="pump-toggle">
                    <input type="checkbox" ${pump.enabled ? 'checked' : ''} 
                           onchange="togglePump(${index}, this.checked)">
                    <span class="slider"></span>
                </label>
            </div>
            <div class="pump-settings">
                <div class="form-group">
                    <label>Kalibracja (ml/s)</label>
                    <input type="number" step="0.1" min="0.1" max="10" 
                           value="${pump.calibration}"
                           onchange="updatePumpCalibration(${index}, this.value)">
                </div>
                <div class="form-group">
                    <label>Dawka (ml)</label>
                    <input type="number" step="0.1" min="0" 
                           value="${pump.dose}"
                           onchange="updatePumpDose(${index}, this.value)">
                </div>
                <div class="form-group">
                    <label>Godzina dozowania</label>
                    <input type="number" min="0" max="23" 
                           value="${pump.schedule_hour}"
                           onchange="updatePumpHour(${index}, this.value)">
                </div>
                <div class="form-group">
                    <label>Dni dozowania</label>
                    <div class="days-select">
                        ${renderDaysSelect(index, pump.schedule_days)}
                    </div>
                </div>
            </div>
        `;
        container.appendChild(card);
    });
}

// Renderowanie wyboru dni
function renderDaysSelect(pumpIndex, scheduleDays) {
    const days = ['N', 'Pn', 'Wt', 'Śr', 'Cz', 'Pt', 'Sb'];
    return days.map((day, i) => `
        <label class="day-checkbox">
            <input type="checkbox" 
                   ${(scheduleDays & (1 << i)) ? 'checked' : ''}
                   onchange="updatePumpDays(${pumpIndex}, ${i}, this.checked)">
            ${day}
        </label>
    `).join('');
}

// Aktualizacja konfiguracji pompy
async function updatePumpConfig(pumpIndex, updates) {
    pumpsData[pumpIndex] = { ...pumpsData[pumpIndex], ...updates };
    try {
        await fetch('/api/pumps', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ pumps: pumpsData })
        });
    } catch (error) {
        console.error('Błąd aktualizacji konfiguracji pompy:', error);
    }
}

// Funkcje aktualizacji poszczególnych parametrów pompy
function togglePump(index, enabled) {
    updatePumpConfig(index, { enabled });
}

function updatePumpCalibration(index, calibration) {
    updatePumpConfig(index, { calibration: parseFloat(calibration) });
}

function updatePumpDose(index, dose) {
    updatePumpConfig(index, { dose: parseFloat(dose) });
}

function updatePumpHour(index, hour) {
    updatePumpConfig(index, { schedule_hour: parseInt(hour) });
}

function updatePumpDays(index, dayIndex, checked) {
    const currentDays = pumpsData[index].schedule_days;
    const newDays = checked ? 
        currentDays | (1 << dayIndex) : 
        currentDays & ~(1 << dayIndex);
    updatePumpConfig(index, { schedule_days: newDays });
}

// Obsługa konfiguracji MQTT
async function loadMQTTConfig() {
    try {
        const response = await fetch('/api/mqtt');
        const config = await response.json();
        document.getElementById('mqtt-server').value = config.server;
        document.getElementById('mqtt-port').value = config.port;
        document.getElementById('mqtt-user').value = config.user;
        document.getElementById('mqtt-password').value = config.password;
    } catch (error) {
        console.error('Błąd pobierania konfiguracji MQTT:', error);
    }
}

async function saveMQTTConfig() {
    const config = {
        server: document.getElementById('mqtt-server').value,
        port: parseInt(document.getElementById('mqtt-port').value),
        user: document.getElementById('mqtt-user').value,
        password: document.getElementById('mqtt-password').value
    };

    try {
        const response = await fetch('/api/mqtt', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(config)
        });
        
        if (response.ok) {
            alert('Konfiguracja MQTT zapisana');
        } else {
            alert('Błąd zapisywania konfiguracji MQTT');
        }
    } catch (error) {
        console.error('Błąd zapisywania konfiguracji MQTT:', error);
        alert('Błąd zapisywania konfiguracji MQTT');
    }
}

// Aktualizacja informacji systemowych
function updateSystemInfo() {
    // TODO: Dodać endpoint API dla informacji systemowych
    const uptimeElement = document.getElementById('uptime');
    const mqttStatusElement = document.getElementById('mqtt-status');
    
    fetch('/api/system')
        .then(response => response.json())
        .then(data => {
            uptimeElement.textContent = data.uptime;
            mqttStatusElement.textContent = data.mqtt_connected ? 'Połączono' : 'Niepołączono';
            document.getElementById('connection-status').className = 
                data.mqtt_connected ? 'connected' : 'disconnected';
        })
        .catch(error => console.error('Błąd pobierania informacji systemowych:', error));
}
