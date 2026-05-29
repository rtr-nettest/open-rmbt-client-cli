use std::fs;
use std::path::PathBuf;

fn uuid_file_path() -> Option<PathBuf> {
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)?;
    Some(home.join(".rmbt_client_uuid"))
}

/// Load a persisted UUID from disk.  Returns None if no file exists yet.
pub fn load() -> Option<String> {
    let path = uuid_file_path()?;
    let s = fs::read_to_string(&path).ok()?;
    let uuid = s.trim().to_string();
    if uuid.is_empty() { None } else { Some(uuid) }
}

/// Persist a UUID received from the control server.
pub fn save(uuid: &str) {
    let path = match uuid_file_path() {
        Some(p) => p,
        None => {
            eprintln!("Warning: cannot determine home directory; UUID will not be persisted");
            return;
        }
    };
    match fs::write(&path, uuid) {
        Ok(_)  => println!("Client UUID saved: {uuid}\n  ({})", path.display()),
        Err(e) => eprintln!("Warning: could not save UUID to {}: {e}", path.display()),
    }
}
