use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};

/// POST `body` as JSON to `url`, returning the HTTP status code and the response
/// body as a string.
///
/// The agent is configured with `http_status_as_error(false)` so that 4xx/5xx
/// responses come back as `Ok` (with their body available) rather than as a
/// transport error, letting callers report the server's error text.
fn post_json(url: &str, body: &serde_json::Value) -> Result<(u16, String)> {
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .http_status_as_error(false)
        .build()
        .into();

    let resp = agent
        .post(url)
        .send_json(body)
        .with_context(|| format!("request to {url} failed"))?;
    let status = resp.status().as_u16();
    let text = resp
        .into_body()
        .read_to_string()
        .context("failed to read response body")?;
    Ok((status, text))
}

/// Parameters returned by the control server for a single test session.
pub struct TestParams {
    pub token:          String,
    pub test_uuid:      Option<String>,
    pub open_test_uuid: Option<String>,
    pub server_addr:    String,
    pub server_port:    u16,
    pub encryption:     bool,
    pub duration:       u32,
    pub num_threads:    u32,
    pub wait:           u32,
    pub server_type:    String,
}

// ─── Wire types ───────────────────────────────────────────────────────────────

#[derive(Serialize)]
struct SettingsRequest<'a> {
    name:        &'a str,
    #[serde(rename = "type")]
    client_type: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    uuid:        Option<&'a str>,
    language:    &'a str,
    timezone:    &'a str,
    #[serde(rename = "softwareRevision")]
    software_revision: &'a str,
    #[serde(rename = "softwareVersionName")]
    software_version_name: &'a str,
    terms_and_conditions_accepted: bool,
}

#[derive(Deserialize)]
struct SettingEntry {
    uuid: Option<String>,
}

#[derive(Deserialize)]
struct SettingsResponse {
    #[serde(default)]
    settings: Vec<SettingEntry>,
}

#[derive(Serialize)]
struct TestRequest<'a> {
    #[serde(skip_serializing_if = "Option::is_none")]
    uuid:                Option<&'a str>,
    client:              &'a str,
    version:             &'a str,
    #[serde(rename = "type")]
    client_type:         &'a str,
    #[serde(rename = "softwareVersion")]
    software_version:    &'a str,
    #[serde(rename = "softwareRevision")]
    software_revision:   &'a str,
    language:            &'a str,
    timezone:            &'a str,
    time:                u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    capabilities:        Option<serde_json::Value>,
}

#[derive(Deserialize)]
struct TestResponse {
    test_token:             Option<String>,
    test_uuid:              Option<String>,
    open_test_uuid:         Option<String>,
    test_server_address:    Option<String>,
    test_server_port:       Option<serde_json::Value>,
    test_server_encryption: Option<bool>,
    test_server_type:       Option<String>,
    #[serde(default, deserialize_with = "de_opt_u32")]
    test_duration:          Option<u32>,
    #[serde(default, deserialize_with = "de_opt_u32")]
    test_numthreads:        Option<u32>,
    #[serde(default, deserialize_with = "de_opt_u32")]
    test_wait:              Option<u32>,
    #[serde(default)]
    error:                  Vec<String>,
}

fn de_opt_u32<'de, D>(de: D) -> std::result::Result<Option<u32>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let v = <serde_json::Value as serde::Deserialize>::deserialize(de)?;
    match v {
        serde_json::Value::Number(n) => Ok(n.as_u64().map(|x| x as u32)),
        serde_json::Value::String(s) => Ok(s.parse().ok()),
        serde_json::Value::Null => Ok(None),
        other => Err(serde::de::Error::custom(
            format!("expected number or string for u32 field, got {other}"),
        )),
    }
}

// ─── Result submission types ──────────────────────────────────────────────────

#[derive(Serialize)]
pub struct PingItem {
    pub value:        u64,
    pub value_server: u64,
    pub time_ns:      u64,
}

#[derive(Serialize)]
pub struct SpeedItem {
    pub direction: String,
    pub thread:    usize,
    pub time:      u64,
    pub bytes:     u64,
}

#[derive(Serialize)]
pub struct TestResultSubmission {
    pub client_language:         String,
    pub client_name:             String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub client_uuid:             Option<String>,
    pub client_version:          String,
    /// Version announced by the measurement server's greeting; `null` when the
    /// server sent no version.
    pub client_software_version: Option<String>,
    #[serde(rename = "geoLocations")]
    pub geo_locations:           Vec<serde_json::Value>,
    pub model:                   String,
    pub network_type:            u32,
    pub platform:                String,
    pub product:                 String,
    pub pings:                   Vec<PingItem>,
    pub test_bytes_download:     u64,
    pub test_bytes_upload:       u64,
    pub test_nsec_download:      u64,
    pub test_nsec_upload:        u64,
    pub test_num_threads:        usize,
    pub num_threads_ul:          usize,
    pub test_ping_shortest:      u64,
    pub test_speed_download:     u64,
    pub test_speed_upload:       u64,
    pub test_token:              String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub test_uuid:               Option<String>,
    pub time:                    u64,
    pub timezone:                String,
    #[serde(rename = "type")]
    pub client_type:             String,
    pub version_code:            String,
    pub speed_detail:            Vec<SpeedItem>,
    pub user_server_selection:   bool,
    pub test_status:             String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub test_port_remote:        Option<u16>,
}

// ─── Public API ───────────────────────────────────────────────────────────────

/// POST `{host}/RMBTControlServer/settings` to register (or re-identify) the client.
/// Returns the client UUID assigned by the server.
pub fn request_settings(
    host:              &str,
    uuid:              Option<&str>,
    software_version:  &str,
    software_revision: &str,
    debug:             bool,
) -> Result<String> {
    let base = host.trim_end_matches('/');
    let url  = format!("{base}/RMBTControlServer/settings");

    let body = serde_json::to_value(SettingsRequest {
        name:                          "RMBT",
        client_type:                   "DESKTOP",
        uuid,
        language:                      "en",
        timezone:                      "UTC",
        software_revision,
        software_version_name:         software_version,
        terms_and_conditions_accepted: true,
    })?;

    if debug {
        eprintln!("[debug] POST {url}");
        eprintln!("[debug] settings request body:\n{}", serde_json::to_string_pretty(&body)?);
    }

    let (code, raw) = post_json(&url, &body).context("settings request failed")?;
    if code >= 400 {
        bail!("settings request returned HTTP {code}: {}", raw.trim());
    }

    if debug {
        let pretty = serde_json::from_str::<serde_json::Value>(&raw)
            .map(|v| serde_json::to_string_pretty(&v).unwrap_or_else(|_| raw.clone()))
            .unwrap_or_else(|_| raw.clone());
        eprintln!("[debug] settings response:\n{pretty}");
    }

    let resp: SettingsResponse = serde_json::from_str(&raw)
        .context("failed to parse settings response")?;

    resp.settings
        .into_iter()
        .next()
        .and_then(|e| e.uuid)
        .context("settings response contained no UUID")
}

pub fn request_test(
    host:              &str,
    uuid:              Option<&str>,
    software_version:  &str,
    software_revision: &str,
    use_ws:            bool,
    debug:             bool,
) -> Result<TestParams> {
    let base = host.trim_end_matches('/');
    let url  = format!("{base}/RMBTControlServer/testRequest");

    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64;

    let client_id = if use_ws { "RMBTws" } else { "RMBT" };

    let capabilities = if !use_ws {
        Some(serde_json::json!({ "RMBThttp": true }))
    } else {
        None
    };

    let body = serde_json::to_value(TestRequest {
        uuid,
        client:            client_id,
        version:           "0.9",
        client_type:       "DESKTOP",
        software_version,
        software_revision,
        language:          "en",
        timezone:          "UTC",
        time:              now_ms,
        capabilities,
    })?;

    if debug {
        eprintln!("[debug] POST {url}");
        eprintln!("[debug] request body:\n{}", serde_json::to_string_pretty(&body)?);
    }

    let (code, raw) = post_json(&url, &body).context("control server request failed")?;
    if code >= 400 {
        if debug { eprintln!("[debug] HTTP {code} response:\n{raw}"); }
        bail!("control server returned HTTP {code}: {}", raw.trim());
    }

    if debug {
        let pretty = serde_json::from_str::<serde_json::Value>(&raw)
            .map(|v| serde_json::to_string_pretty(&v).unwrap_or_else(|_| raw.clone()))
            .unwrap_or_else(|_| raw.clone());
        eprintln!("[debug] response body:\n{pretty}");
    }

    let resp: TestResponse = serde_json::from_str(&raw)
        .context("failed to parse control server JSON")?;

    if !resp.error.is_empty() {
        bail!("control server error(s): {}", resp.error.join("; "));
    }

    let server_port = match &resp.test_server_port {
        Some(serde_json::Value::Number(n)) => n.as_u64().unwrap_or(443) as u16,
        Some(serde_json::Value::String(s)) => s.parse().unwrap_or(443),
        _ => 443,
    };

    Ok(TestParams {
        token:          resp.test_token.context("missing test_token")?,
        test_uuid:      resp.test_uuid,
        open_test_uuid: resp.open_test_uuid,
        server_addr:    resp.test_server_address.context("missing test_server_address")?,
        server_port,
        encryption:     resp.test_server_encryption.unwrap_or(true),
        duration:       resp.test_duration.unwrap_or(10),
        num_threads:    resp.test_numthreads.unwrap_or(4),
        wait:           resp.test_wait.unwrap_or(0),
        server_type:    resp.test_server_type.unwrap_or_default(),
    })
}

/// POST `{host}/RMBTControlServer/result`.
/// Submission errors are logged as warnings but never propagate — the test
/// has already completed and the data should not be discarded.
pub fn submit_result(host: &str, result: &TestResultSubmission, debug: bool) -> Result<()> {
    let base = host.trim_end_matches('/');
    let url  = format!("{base}/RMBTControlServer/result");

    let body = serde_json::to_value(result)?;
    if debug {
        eprintln!("[debug] POST {url}");
        eprintln!("[debug] result body:\n{}", serde_json::to_string_pretty(&body)?);
    }

    match post_json(&url, &body) {
        Ok((code, resp)) if code < 400 => {
            if debug { eprintln!("[debug] result response:\n{resp}"); }
        }
        Ok((code, resp)) => {
            if debug { eprintln!("[debug] HTTP {code} response:\n{resp}"); }
            eprintln!("Warning: result submission returned HTTP {code}");
        }
        Err(e) => {
            eprintln!("Warning: result submission failed: {e}");
        }
    }

    Ok(())
}
