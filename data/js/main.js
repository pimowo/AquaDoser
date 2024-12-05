// Aktualizacja czasu
function updateTime() {
    fetch('/api/time')
        .then(response => response.json())
        .then(data => {
            document.getElementById('time-display').textContent = data.time;
            document.getElementById('date-display').textContent = data.date;
            document.getElementById('timezone-badge').textContent = data.timezone;
        })
        .catch(error => console.error('Error:', error));
}

// Obsługa pomp
function setupPumpControls() {
    const pumps = [1, 2];
    pumps.forEach(pumpId => {
        const checkbox = document.getElementById(`pump${pumpId}`);
        checkbox.addEventListener('change', () => {
            fetch(`/api/pump/${pumpId}/${checkbox.checked ? 'on' : 'off'}`)
                .then(response => response.text())
                .then(result => {
                    console.log(`Pump ${pumpId}: ${result}`);
                })
                .catch(error => console.error('Error:', error));
        });
    });
}

// Aktualizacja stanu pomp
function updatePumpStates() {
    fetch('/api/pumps')
        .then(response => response.json())
        .then(data => {
            document.getElementById('pump1').checked = data.pump1;
            document.getElementById('pump2').checked = data.pump2;
        })
        .catch(error => console.error('Error:', error));
}

// Harmonogram
function updateSchedule() {
    fetch('/api/schedule')
        .then(response => response.json())
        .then(data => {
            const scheduleDiv = document.getElementById('schedule');
            scheduleDiv.innerHTML = ''; // Wyczyść obecny harmonogram
            // Tu dodaj kod wyświetlania harmonogramu
        })
        .catch(error => console.error('Error:', error));
}

// Inicjalizacja
document.addEventListener('DOMContentLoaded', () => {
    setupPumpControls();
    updateTime();
    updatePumpStates();
    updateSchedule();
    
    // Aktualizuj czas co sekundę
    setInterval(updateTime, 1000);
    // Aktualizuj stan pomp co 5 sekund
    setInterval(updatePumpStates, 5000);
});
