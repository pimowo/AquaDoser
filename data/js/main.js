var socket;
window.onload = function() {
    // Inicjalizacja zegara
    updateClock();
    setInterval(updateClock, 1000);

    // Obsługa formularza MQTT
    document.querySelector('form[action="/save-mqtt"]').addEventListener('submit', function(e) {
        e.preventDefault();
        
        var formData = new FormData(this);
        fetch('/save-mqtt', {
            method: 'POST',
            body: formData
        }).then(response => {
            if (!response.ok) {
                throw new Error('Network response was not ok');
            }
            showMessage('Zapisano ustawienia MQTT', 'success');
        }).catch(error => {
            showMessage('Błąd podczas zapisywania MQTT!', 'error');
        });
    });

    // Obsługa formularza pomp
    document.querySelector('form[action="/save-pumps"]').addEventListener('submit', function(e) {
        e.preventDefault();
        
        var formData = new FormData(this);
        fetch('/save-pumps', {
            method: 'POST',
            body: formData
        }).then(response => {
            if (!response.ok) {
                throw new Error('Network response was not ok');
            }
            showMessage('Zapisano ustawienia pomp', 'success');
        }).catch(error => {
            showMessage('Błąd podczas zapisywania pomp!', 'error');
        });
    });

    // Inicjalizacja WebSocket
    socket = new WebSocket('ws://' + window.location.hostname + ':81/');
    socket.onmessage = function(event) {
        var message = event.data;
        
        if (message.startsWith('update:')) {
            handleUpdateMessage(message);
        } 
        else if (message.startsWith('save:')) {
            var parts = message.split(':');
            var type = parts[1];
            var text = parts[2];
            showMessage(text, type);
        }
        else if (message.startsWith('pump:')) {
            handlePumpMessage(message);
        }
    };

    // Dodanie obsługi przycisków testu pomp
    document.querySelectorAll('.test-pump').forEach(button => {
        button.addEventListener('click', function() {
            var pumpId = this.dataset.pump;
            testPump(pumpId);
        });
    });
};

// Obsługa wiadomości aktualizacji
function handleUpdateMessage(message) {
    if (message.startsWith('update:error:')) {
        document.getElementById('update-progress').style.display = 'none';
        showMessage(message.split(':')[2], 'error');
        return;
    }
    
    var percentage = message.split(':')[1];
    var progressBar = document.getElementById('progress-bar');
    var progressContainer = document.getElementById('update-progress');
    
    progressContainer.style.display = 'block';
    progressBar.style.width = percentage + '%';
    progressBar.textContent = percentage + '%';
    
    if (percentage == '100') {
        progressContainer.style.display = 'none';
        showMessage('Aktualizacja zakończona pomyślnie! Trwa restart urządzenia...', 'success');
        setTimeout(function() {
            window.location.reload();
        }, 3000);
    }
}

// Obsługa wiadomości pompy
function handlePumpMessage(message) {
    var parts = message.split(':');
    var pumpId = parts[1];
    var status = parts[2];
    
    // Aktualizuj status pompy w interfejsie
    var statusElement = document.querySelector(`.pump-status[data-pump="${pumpId}"]`);
    if (statusElement) {
        statusElement.className = `pump-status ${status}`;
        statusElement.textContent = status === 'active' ? 'Aktywna' : 'Nieaktywna';
    }
}

// Funkcja do aktualizacji zegara
function updateClock() {
    fetch('/api/time')
        .then(response => response.json())
        .then(data => {
            document.getElementById('time').innerHTML = 
                `${String(data.hour).padStart(2, '0')}:${String(data.minute).padStart(2, '0')}:${String(data.second).padStart(2, '0')}`;
            document.getElementById('date').innerHTML = 
                `${String(data.day).padStart(2, '0')}/${String(data.month).padStart(2, '0')}/${data.year}`;
            document.getElementById('timezone').innerHTML = 
                `${data.isDST ? 'Letni' : 'Zimowy'}`;
        })
        .catch(error => console.error('Error fetching time:', error));
}

// Funkcja do testowania pompy
function testPump(pumpId) {
    fetch(`/test-pump/${pumpId}`, {
        method: 'POST'
    }).then(response => {
        if (!response.ok) {
            throw new Error('Network response was not ok');
        }
        showMessage(`Testowanie pompy ${parseInt(pumpId) + 1}`, 'success');
    }).catch(error => {
        showMessage(`Błąd podczas testowania pompy ${parseInt(pumpId) + 1}!`, 'error');
    });
}

// Funkcja restartu urządzenia
function rebootDevice() {
    if(confirm('Czy na pewno chcesz zrestartować urządzenie?')) {
        fetch('/reboot', {method: 'POST'})
        .then(response => {
            if (!response.ok) throw new Error('Network response was not ok');
            showMessage('Urządzenie zostanie zrestartowane...', 'success');
            setTimeout(() => { window.location.reload(); }, 3000);
        })
        .catch(error => {
            showMessage('Błąd podczas restartowania urządzenia!', 'error');
        });
    }
}

// Funkcja resetu do ustawień fabrycznych
function factoryReset() {
    if(confirm('Czy na pewno chcesz przywrócić ustawienia fabryczne? Spowoduje to utratę wszystkich ustawień.')) {
        fetch('/factory-reset', {method: 'POST'})
        .then(response => {
            if (!response.ok) throw new Error('Network response was not ok');
            showMessage('Przywracanie ustawień fabrycznych...', 'success');
            setTimeout(() => { window.location.reload(); }, 3000);
        })
        .catch(error => {
            showMessage('Błąd podczas przywracania ustawień!', 'error');
        });
    }
}

// Funkcja pokazująca komunikaty
function showMessage(text, type) {
    var oldMessages = document.querySelectorAll('.message');
    oldMessages.forEach(function(msg) {
        msg.remove();
    });
    
    var messageBox = document.createElement('div');
    messageBox.className = 'message ' + type;
    messageBox.innerHTML = text;
    document.body.appendChild(messageBox);
    
    // Pokazanie komunikatu
    requestAnimationFrame(() => {
        messageBox.style.opacity = '1';
    });
    
    // Ukrycie i usunięcie komunikatu
    setTimeout(function() {
        messageBox.style.opacity = '0';
        setTimeout(function() {
            messageBox.remove();
        }, 300);
    }, 3000);
}
