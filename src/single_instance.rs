//! Process single-instance guard backed by an advisory file lock.

use crate::config::runtime_directory;
use fs2::FileExt as _;
use std::fs::{File, OpenOptions};

pub struct SingleInstance {
    file: File,
}

impl SingleInstance {
    pub fn acquire() -> anyhow::Result<Option<Self>> {
        let path = runtime_directory()?.join("instance.lock");
        let file = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(path)?;
        match file.try_lock_exclusive() {
            Ok(()) => Ok(Some(Self { file })),
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => Ok(None),
            Err(error) => Err(error.into()),
        }
    }
}

impl Drop for SingleInstance {
    fn drop(&mut self) {
        let _ = fs2::FileExt::unlock(&self.file);
    }
}