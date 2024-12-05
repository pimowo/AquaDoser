function updateTime() {
    fetch('/api/time')
        .then(response => response.json())
        .then(data => {
            document.querySelector('.time-display').textContent = data.time;
            document.querySelector('.date-display').textContent = data.date;
            document.querySelector('.timezone-badge').textContent = data.timezone;
        })
        .catch(error => console.error('Error:', error));
}

// Aktualizuj czas co sekundę
setInterval(updateTime, 1000);
// Pierwsze wywołanie
updateTime();
