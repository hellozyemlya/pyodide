import { PyodideModule } from "./types";

/**
 * @private
 */
async function syncfs(m: PyodideModule, direction: boolean): Promise<void> {
  return new Promise((resolve, reject) => {
    m.FS.syncfs(direction, (err: any) => {
      if (err) {
        reject(err);
      } else {
        resolve();
      }
    });
  });
}

/**
 * @private
 */
export async function syncLocalToRemote(m: PyodideModule): Promise<void> {
  return await syncfs(m, false);
}

/**
 * @private
 */
export async function syncRemoteToLocal(m: PyodideModule): Promise<void> {
  return await syncfs(m, true);
}

/**
 * @private
 */
export function initializeNativeFS(module: PyodideModule) {

}

const getFsHandles = async (dirHandle: any) => {
  const handles: any = [];

  async function collect(curDirHandle: any) {
    for await (const entry of curDirHandle.values()) {
      handles.push(entry);
      if (entry.kind === "directory") {
        await collect(entry);
      }
    }
  }

  await collect(dirHandle);

  const result = new Map();
  result.set(".", dirHandle);
  for (const handle of handles) {
    const relativePath = (await dirHandle.resolve(handle)).join("/");
    result.set(relativePath, handle);
  }
  return result;
};
