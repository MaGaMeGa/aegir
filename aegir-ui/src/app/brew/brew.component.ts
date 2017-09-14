import { Component, OnInit } from '@angular/core';
import 'rxjs/add/operator/switchMap';

import { Program } from '../programs/program';
import { ApiService } from '../api.service';

@Component({
  selector: 'app-brew',
  templateUrl: './brew.component.html',
  styleUrls: ['./brew.component.css']
})
export class BrewComponent implements OnInit {
    public program: Program;

    public sensors = [];

    public state: string = null;
    public needmalt = false;
    public targettemp = 0;
    public mashstep = null;

    // chart
    public brewChartData: Array<any> = [
	{data: [], label: 'Mash Tun'},
	{data: [], label: 'RIMS Tube'}];
    public brewChartLabels: Array<any> = [];
    public brewChartOptions: any = {
	responsive: true,
	spanGaps: true,
	title: {
	    display: true,
	    text: 'RIMS and MashTun temperature'
	},
	scales: {
	    xAxes: [{
		ticks: {
		    callback: function (value, index, values) {
			return value + 's';
		    }
		}
	    }],
	    yAxes: [{
		ticks: {
		    beginAtZero: true
		}
	    }]
	}
    };
    public brewChartLegend:boolean = true;
    public brewChartType:string = 'line';
    public brewChartColors: Array<any> = [
	'blue', 'red'
    ];

    constructor(private api: ApiService) {
    }

    ngOnInit() {
	this.api.getState().subscribe(data => {
	    this.updateState(data);
	});
	this.api.getTempHistory().subscribe(
	    data => {
		this.updateTempHistory(data);
	    });
    }

    updateState(data) {
	//console.log(data);
	this.sensors = [];
	this.state = data['state'];
	this.targettemp = data['targettemp'];
	if ( 'mashstep' in data ) {
	    this.mashstep = data['mashstep'];
	} else {
	    this.mashstep = null;
	}
	for (let key in data['currtemp']) {
	    let temp = data['currtemp'][key];
	    if ( temp > 1000 ) temp = 0.0;
	    this.sensors.push({'sensor': key, 'temp': temp});
	}
    }

    swissCheese(a) {
	let size:number = a.length;
	for ( let i in a ) {
	    let ni:number = +i;
	    let age:number = size-ni;
	    if ( age > 3600 ) {
		if ( ni%180 != 0 ) a[i] = null;
	    } else if ( age > 600 ) {
		if ( ni%60 != 0 ) a[i] = null;
	    } else if ( age > 180 ) {
		if ( ni%10 != 0 ) a[i] = null;
	    }
	}
	return a;
    }

    updateTempHistory(data) {
	//console.log('Updating temphistory with ', data);

	// first clear the data

	let mt:Array<number> = [];
	let rims:Array<number> = [];

	for (let key in data['readings']) {
	    if ( key == 'MashTun' ) {
		mt = data['readings'][key];
	    } else if ( key == 'RIMS' ) {
		rims = data['readings'][key];
	    }
	}
	this.brewChartData = [
	    {data: this.swissCheese(mt), label: 'Mash Tun'},
	    {data: this.swissCheese(rims), label: 'RIMS Tube'}
	];
	this.brewChartLabels.length = 0;
	for ( let i of data['timestamps'] ) {
	    this.brewChartLabels.push(i);
	}
	//console.log('chartdata', this.brewChartData);
	//console.log('chart labels', this.brewChartLabels);
    }

    onHasMalts() {
	this.api.hasMalt().subscribe();
	//console.log("HasMalts pushed");
    }

}
