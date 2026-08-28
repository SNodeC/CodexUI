import {StrictMode} from "react";
import {createRoot} from "react-dom/client";
import {App} from "./app/App.js";
import {BrowserFrontendSession} from "./app/BrowserFrontendSession.js";
import "./styles.css";

const root = document.getElementById("root");
if (!root) throw new Error("CodexUI root element is missing");
const session = new BrowserFrontendSession();
createRoot(root).render(<StrictMode><App session={session} /></StrictMode>);
