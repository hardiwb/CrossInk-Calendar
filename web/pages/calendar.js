const monthNames = ['January', 'February', 'March', 'April', 'May', 'June',
  'July', 'August', 'September', 'October', 'November', 'December'];
const today = new Date();
let viewYear = today.getFullYear();
let viewMonth = today.getMonth() + 1;
let selectedDay = today.getDate();
let entryDays = new Set();

function queryDate(day) {
  return 'year=' + viewYear + '&month=' + viewMonth + '&day=' + day;
}

function showMessage(text, error) {
  const element = document.getElementById('message');
  element.textContent = text;
  element.className = 'message ' + (error ? 'error' : 'success');
}

function setEditorEnabled(enabled) {
  document.getElementById('entry-message').disabled = !enabled;
  document.getElementById('save-button').disabled = !enabled;
  document.getElementById('delete-button').disabled = !enabled || !entryDays.has(selectedDay);
}

function updateCount() {
  const length = new TextEncoder().encode(document.getElementById('entry-message').value).length;
  document.getElementById('character-count').textContent = length + ' / 2048 bytes';
  document.getElementById('save-button').disabled = !selectedDay || length === 0 || length > 2048;
}

function renderCalendar() {
  document.getElementById('month-title').textContent = monthNames[viewMonth - 1] + ' ' + viewYear;
  const grid = document.getElementById('calendar-grid');
  grid.innerHTML = '';
  const first = new Date(viewYear, viewMonth - 1, 1);
  const mondayOffset = (first.getDay() + 6) % 7;
  const count = new Date(viewYear, viewMonth, 0).getDate();
  for (let i = 0; i < mondayOffset; i++) {
    const blank = document.createElement('span');
    blank.className = 'blank-day';
    grid.appendChild(blank);
  }
  for (let day = 1; day <= count; day++) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'day';
    if (day === selectedDay) button.classList.add('selected');
    if (entryDays.has(day)) button.classList.add('has-entry');
    if (day === today.getDate() && viewMonth === today.getMonth() + 1 && viewYear === today.getFullYear()) {
      button.classList.add('today');
    }
    button.textContent = day;
    button.onclick = function() { selectDay(day); };
    grid.appendChild(button);
  }
}

async function loadMonth() {
  setEditorEnabled(false);
  try {
    const response = await fetch('/api/calendar?year=' + viewYear + '&month=' + viewMonth);
    if (!response.ok) throw new Error('Could not load this month.');
    const data = await response.json();
    entryDays = new Set(data.days || []);
    renderCalendar();
    if (selectedDay) await loadEntry();
  } catch (error) {
    showMessage(error.message, true);
  }
}

async function selectDay(day) {
  selectedDay = day;
  renderCalendar();
  await loadEntry();
}

async function loadEntry() {
  const title = monthNames[viewMonth - 1] + ' ' + selectedDay + ', ' + viewYear;
  document.getElementById('selected-title').textContent = title;
  document.getElementById('entry-state').textContent = 'Loading...';
  setEditorEnabled(false);
  try {
    const response = await fetch('/api/calendar/entry?' + queryDate(selectedDay));
    if (!response.ok) throw new Error('Could not load the entry.');
    const data = await response.json();
    document.getElementById('entry-message').value = data.message || '';
    document.getElementById('entry-state').textContent = data.exists ? 'Saved on device' : 'No entry yet';
    setEditorEnabled(true);
    updateCount();
  } catch (error) {
    document.getElementById('entry-state').textContent = '';
    showMessage(error.message, true);
  }
}

function moveMonth(delta) {
  const next = new Date(viewYear, viewMonth - 1 + delta, 1);
  const nextYear = next.getFullYear();
  if (nextYear < 2024 || nextYear > 2099) return;
  viewYear = nextYear;
  viewMonth = next.getMonth() + 1;
  selectedDay = 1;
  loadMonth();
}

async function saveEntry() {
  const message = document.getElementById('entry-message').value.trim();
  const byteLength = new TextEncoder().encode(message).length;
  if (!message || byteLength > 2048) {
    showMessage('Entries must contain 1 to 2048 bytes of text.', true);
    return;
  }
  setEditorEnabled(false);
  const body = new URLSearchParams({year: viewYear, month: viewMonth, day: selectedDay, message: message});
  try {
    const response = await fetch('/api/calendar/entry', {method: 'POST', body: body});
    if (!response.ok) throw new Error(await response.text() || 'Could not save the entry.');
    entryDays.add(selectedDay);
    renderCalendar();
    document.getElementById('entry-state').textContent = 'Saved on device';
    showMessage('Calendar entry saved.', false);
  } catch (error) {
    showMessage(error.message, true);
  }
  setEditorEnabled(true);
  updateCount();
}

async function deleteEntry() {
  if (!entryDays.has(selectedDay) || !confirm('Delete this calendar entry?')) return;
  setEditorEnabled(false);
  const body = new URLSearchParams({year: viewYear, month: viewMonth, day: selectedDay});
  try {
    const response = await fetch('/api/calendar/entry/delete', {method: 'POST', body: body});
    if (!response.ok) throw new Error(await response.text() || 'Could not delete the entry.');
    entryDays.delete(selectedDay);
    document.getElementById('entry-message').value = '';
    document.getElementById('entry-state').textContent = 'No entry yet';
    renderCalendar();
    showMessage('Calendar entry deleted.', false);
  } catch (error) {
    showMessage(error.message, true);
  }
  setEditorEnabled(true);
  updateCount();
}

document.getElementById('entry-message').addEventListener('input', updateCount);
loadMonth();
