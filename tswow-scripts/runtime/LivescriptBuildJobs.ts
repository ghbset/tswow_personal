import * as os from "os";

export function getLivescriptBuildJobs(
    override = process.env.CMAKE_BUILD_PARALLEL_LEVEL,
    cpuCount = os.cpus().length,
    memoryBytes = os.totalmem()
): number {
    if (override !== undefined && override.trim() !== '') {
        const value = override.trim();
        const jobs = Number(value);
        if (!/^[0-9]+$/.test(value) || !Number.isSafeInteger(jobs) || jobs < 1) {
            throw new Error('CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer');
        }
        return jobs;
    }
    const memoryJobs = Math.floor(memoryBytes / (2 * 1024 * 1024 * 1024));
    return Math.max(1, Math.min(8, cpuCount - 1, memoryJobs));
}
