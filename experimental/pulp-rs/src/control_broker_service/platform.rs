use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct CommandRequest {
    pub(super) program: PathBuf,
    pub(super) args: Vec<String>,
    pub(super) cwd: Option<PathBuf>,
    pub(super) timeout: Duration,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct CommandOutput {
    pub(super) code: i32,
    pub(super) stdout: String,
    pub(super) stderr: String,
    pub(super) timed_out: bool,
}

pub(super) trait CommandRunner {
    fn run(&self, request: &CommandRequest) -> io::Result<CommandOutput>;
}

#[derive(Debug, Clone, Copy)]
pub(super) struct SystemCommandRunner;

impl CommandRunner for SystemCommandRunner {
    fn run(&self, request: &CommandRequest) -> io::Result<CommandOutput> {
        let mut command = Command::new(&request.program);
        command
            .args(&request.args)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        if let Some(cwd) = &request.cwd {
            command.current_dir(cwd);
        }
        let mut child = command.spawn()?;
        let deadline = Instant::now() + request.timeout;
        loop {
            if child.try_wait()?.is_some() {
                let output = child.wait_with_output()?;
                return Ok(CommandOutput {
                    code: output.status.code().unwrap_or(1),
                    stdout: String::from_utf8_lossy(&output.stdout).into_owned(),
                    stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
                    timed_out: false,
                });
            }
            if Instant::now() >= deadline {
                child.kill()?;
                let output = child.wait_with_output()?;
                return Ok(CommandOutput {
                    code: output.status.code().unwrap_or(1),
                    stdout: String::from_utf8_lossy(&output.stdout).into_owned(),
                    stderr: String::from_utf8_lossy(&output.stderr).into_owned(),
                    timed_out: true,
                });
            }
            thread::sleep(Duration::from_millis(10));
        }
    }
}

pub(super) trait FileSystem {
    fn is_file(&self, path: &Path) -> bool;
    fn is_symlink(&self, path: &Path) -> io::Result<bool>;
    fn read(&self, path: &Path) -> io::Result<Vec<u8>>;
    fn create_dir_all(&self, path: &Path) -> io::Result<()>;
    fn write(&self, path: &Path, contents: &[u8]) -> io::Result<()>;
    fn rename(&self, from: &Path, to: &Path) -> io::Result<()>;
    fn remove_file(&self, path: &Path) -> io::Result<()>;
    fn set_owner_private(&self, path: &Path) -> io::Result<()>;
}

#[derive(Debug, Clone, Copy)]
pub(super) struct SystemFileSystem;

impl FileSystem for SystemFileSystem {
    fn is_file(&self, path: &Path) -> bool {
        path.is_file()
    }

    fn is_symlink(&self, path: &Path) -> io::Result<bool> {
        match fs::symlink_metadata(path) {
            Ok(metadata) => Ok(metadata.file_type().is_symlink()),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(false),
            Err(error) => Err(error),
        }
    }

    fn read(&self, path: &Path) -> io::Result<Vec<u8>> {
        fs::read(path)
    }

    fn create_dir_all(&self, path: &Path) -> io::Result<()> {
        fs::create_dir_all(path)
    }

    fn write(&self, path: &Path, contents: &[u8]) -> io::Result<()> {
        fs::write(path, contents)
    }

    fn rename(&self, from: &Path, to: &Path) -> io::Result<()> {
        fs::rename(from, to)
    }

    fn remove_file(&self, path: &Path) -> io::Result<()> {
        fs::remove_file(path)
    }

    fn set_owner_private(&self, path: &Path) -> io::Result<()> {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(path, fs::Permissions::from_mode(0o600))
        }
        #[cfg(not(unix))]
        {
            let _ = path;
            Ok(())
        }
    }
}
