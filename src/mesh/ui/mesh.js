"use strict";
fetch("/mesh/v1/status").then(r => r.json()).then(s => {
	document.getElementById("app").textContent = JSON.stringify(s, null, 2);
});
