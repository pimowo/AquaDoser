var socket;
window.onload = function() {
    document.querySelector('form[action="/save"]').addEventListener('submit', function(e) {
        e.preventDefault();
        
        var formData = new FormData(this);
        fetch('/save', {
            method: 'POST',
            body: formData
        }).then(response => {
            if (!response.ok) {
                throw new Error('Network response was not ok');
            }
        }).catch(error => {
            showMessage('Błąd podczas zapisywania!', 'error');
        });
    });

    socket = new WebSocket('ws://' + window.location.hostname + ':81/');
    socket.onmessage = function(event) {
        var message = event.data;
        
        if (message.startsWith('update:')) {
            if (message.startsWith('update:error:')) {
                document.getElementById('update-progress').style.display = 'none';
                showMessage(message.split(':')[2], 'error');
                return;
            }
            var percentage = message.split(':')[1];
            document.getElementById('update-progress').style.display = 'block';
            document.getElementById('progress-bar').style.width = percentage + '%';
            document.getElementById('progress-bar').textContent = percentage + '%';
            
            if (percentage == '100') {
                document.getElementById('update-progress').style.display = 'none';
                showMessage('Aktualizacja zakończona pomyślnie! Trwa restart urządzenia...', 'success');
                setTimeout(function() {
                    window.location.reload();
                }, 3000);
            }
        } 
        else if (message.startsWith('save:')) {
            var parts = message.split(':');
            var type = parts[1];
            var text = parts[2];
            showMessage(text, type);
        }
    };
};

function rebootDevice() {
    if(confirm('Czy na pewno chcesz zrestartować urządzenie?')) {
        fetch('/reboot', {method: 'POST'}).then(() => {
            showMessage('Urządzenie zostanie zrestartowane...', 'success');
            setTimeout(() => { window.location.reload(); }, 3000);
        });
    }
}

function factoryReset() {
    if(confirm('Czy na pewno chcesz przywrócić ustawienia fabryczne? Spowoduje to utratę wszystkich ustawień.')) {
        fetch('/factory-reset', {method: 'POST'}).then(() => {
            showMessage('Przywracanie ustawień fabrycznych...', 'success');
            setTimeout(() => { window.location.reload(); }, 3000);
        });
    }
}

function showMessage(text, type) {
    var oldMessages = document.querySelectorAll('.message');
    oldMessages.forEach(function(msg) {
        msg.remove();
    });
    
    var messageBox = document.createElement('div');
    messageBox.className = 'message ' + type;
    messageBox.innerHTML = text;
    document.body.appendChild(messageBox);
    
    setTimeout(function() {
        messageBox.style.opacity = '1';
    }, 10);
    
    setTimeout(function() {
        messageBox.style.opacity = '0';
        setTimeout(function() {
            messageBox.remove();
        }, 300);
    }, 3000);
}