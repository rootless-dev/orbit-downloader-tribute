const DEFAULTS = { enabled: false, port: 8697, token: "" };
const $ = id => document.getElementById(id);

chrome.storage.local.get(DEFAULTS).then(c => {
  $("enabled").checked = c.enabled;
  $("port").value = c.port;
  $("token").value = c.token;
});

$("save").addEventListener("click", async () => {
  await chrome.storage.local.set({
    enabled: $("enabled").checked,
    port: parseInt($("port").value, 10) || 8697,
    token: $("token").value.trim(),
  });
  $("status").textContent = "Saved.";
  setTimeout(() => ($("status").textContent = ""), 1500);
});
