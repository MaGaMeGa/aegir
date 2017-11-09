import { Injectable } from '@angular/core';
import { Headers, Http, Response, RequestOptions, URLSearchParams } from '@angular/http';
import { Observable } from 'rxjs/Rx';

import { Program } from './programs/program';

import { Subject } from 'rxjs/Subject';
import 'rxjs/add/operator/map';
import 'rxjs/add/operator/catch';

export class ApiResponse {
    constructor(
	public status: string,
	public uri: string,
	public errors: string[]) {
    }
}

@Injectable()
export class ApiService {

    private static instance: ApiService = null;

    // Observable resources
    private updateSource = new Subject<boolean>();
    // observable streams
    updateAnnounce$ = this.updateSource.asObservable();
    // state timer
    private timer_state;
    // state observable
    private state = new Subject();
    private temphistory = new Subject();

    constructor(private http: Http) {
	if ( ApiService.instance != null ) return ApiService.instance;
	ApiService.instance = this;
	this.timer_state = Observable.timer(2000,1000);
	this.timer_state.subscribe(t => {this.updateState(t)});
    }

    getState(): Observable<{}> {
	return this.state.asObservable();
    }

    getTempHistory(): Observable<{}> {
	return this.temphistory.asObservable();
    }

    updateState(t) {
	let params: URLSearchParams = new URLSearchParams();
	let needhistory = t%5==0;
	params.set('history', needhistory?'yes':'no');

	this.http.get(`/api/brewd/state`, {search: params})
	    .subscribe(res => {
		let rjson = res.json()['data'];
		this.state.next(rjson);
		if ( rjson['temphistory'] ) {
		    this.temphistory.next(rjson['temphistory']);
		}
	    });
    }

    announceUpdate() {
	//console.log('API announcing update');
	this.updateSource.next(true);
    }

    hasMalt(): Observable<{}> {
	let body = JSON.stringify({'command': 'hasMalt'});
	let headers = new Headers({'Content-Type': 'application/json'});
	let options = new RequestOptions({headers: headers});

	console.log('calling /api/brewd/state', body, options);

	return this.http.post('/api/brewd/state', body, options)
	    .map((res:Response) => {
		console.log('Catching result, ', res.status);
		return res.json();
	    });
    }

    getVolume(): Observable<{}> {
	return this.http.get('/api/brewd/state/volume')
	    .map(this.extractData)
	    .catch(this.handleError);
    }

    setVolume(volume): Observable<{}> {
	let body = JSON.stringify({'volume': volume});
	let headers = new Headers({'Content-Type': 'application/json'});
	let options = new RequestOptions({headers: headers});

	console.log("Setting volume to ", volume, body, headers, options);

	return this.http.post('/api/brewd/state/volume', body, options)
	    .map((res:Response) => {
		console.log('Catching result, ', res.status);
		return res.json();
	    })
	    .catch(this.handleError);
    }

    abortBrew(): Observable<{}> {
	let body = JSON.stringify({'command': 'reset'});
	let headers = new Headers({'Content-Type': 'application/json'});
	let options = new RequestOptions({headers: headers});

	console.log('calling /api/brewd/state', body, options);

	return this.http.post('/api/brewd/state', body, options)
	    .map((res:Response) => {
		console.log('Catching result, ', res.status);
		return res.json();
	    });
    }

    getPrograms(): Observable<Program[]> {
	return this.http.get('/api/programs')
	    .map(this.extractData)
	    .catch(this.handleError);
    }

    addProgram(data: Object): Observable<ApiResponse> {
	let body = JSON.stringify(data);
	let headers = new Headers({'Content-Type': 'application/json'});
	let options = new RequestOptions({headers: headers});

	return this.http.post('/api/programs', body, options)
	    .map((res:Response) => {
		//console.log('Catching result, ', res.status);
		return res.json();
	    });
	    //.catch((error:any) => Observable.throw(error.json().error || 'Servererror'));
    }

    saveProgram(data: Object): Observable<ApiResponse> {
	let body = JSON.stringify(data);
	let headers = new Headers({'Content-Type': 'application/json'});
	let options = new RequestOptions({headers: headers});

	return this.http.post(`/api/programs/${data['id']}`, body, options)
	    .map((res:Response) => {
		//console.log('Catching result, ', res.status);
		return res.json();
	    });
	    //.catch((error:any) => Observable.throw(error.json().error || 'Servererror'));
    }

    delProgram(progid: number): Observable<ApiResponse> {
	return this.http.delete(`/api/programs/${progid}`)
	    .map((res:Response) => {
		//console.log('Delete result, ', res.status);
		return res.json();
	    });
	    //.catch((error:any) => Observable.throw(error.json().error || 'Servererror'));
    }

    getProgram(progid: number): Observable<Program> {
	return this.http.get(`/api/programs/${progid}`)
	    .map((res:Response) => res.json());
	    //.catch((error:any) => Observable.throw(error.json().error || 'Servererror'));
    }

    loadProgram(data: Object): Observable<ApiResponse> {
	let body = JSON.stringify(data);
	let headers = new Headers({'Content-Type': 'application/json'});
	let options = new RequestOptions({headers: headers});

	return this.http.post('/api/brewd/program', body, options)
	    .map((res:Response) => {
		//console.log('Catching result, ', res.status);
		return res.json();
	    });
	    //.catch((error:any) => Observable.throw(error.json().error || 'Servererror'));
    }

    private extractData(res: Response) {
	let body = res.json()
	return body.data || {};
    }

    private handleError(error: Response | any ) {
	let errMsg: string;
	if (error instanceof Response) {
	    const body = error.json() || '';
	    const err = body.error || JSON.stringify(body);
	    errMsg = `${error.status} - ${error.statusText || ''} ${err}`;
	} else {
	    errMsg = error.message ? error.message : error.toString();
	}
	console.log(errMsg);
	return Observable.throw(errMsg);
    }

}
