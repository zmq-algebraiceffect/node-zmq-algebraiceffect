export class Client {
    constructor(endpoint: string);
    perform(effect: string, payload: any): Promise<PerformResult>;
    perform(effect: string, payload: any, timeout: number): Promise<PerformResult>;
    perform(effect: string, payload: any, binaries: Buffer[], timeout: number): Promise<PerformResult>;
    close(): void;
}

export class Router {
    constructor(endpoint: string);
    on(effect: string, handler: (ctx: HandlerContext) => void): void;
    off(effect: string): void;
    close(): void;
}

export interface PerformResult {
    id: string;
    value?: any;
    error?: string;
}

export interface HandlerContext {
    id: string;
    effect: string;
    payload: any;
    resume(value?: any): void;
    error(message: string): void;
}
