package com.example.nextu.cmakeappmetro;

import android.Manifest;
import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.content.Context;
import android.content.pm.PackageManager;
import android.content.res.AssetFileDescriptor;
import android.content.res.AssetManager;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Build;
import android.os.Bundle;
import android.support.annotation.Keep;
import android.support.v4.app.ActivityCompat;
import android.support.v4.content.ContextCompat;
import android.util.Log;
import android.view.KeyEvent;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.NumberPicker;
import android.widget.TextView;
import android.support.design.widget.FloatingActionButton;
import android.support.v7.app.AppCompatActivity;
import android.support.v7.widget.Toolbar;
import android.view.View;
import android.view.Menu;
import android.view.MenuItem;
import android.widget.Toast;

import java.io.DataInputStream;
import java.io.IOException;

public class MainActivity extends AppCompatActivity {

    private static final String numerateur[] = { "2","3","4","6","9","12" } ;
    private static final String denominateur[] = { "1","2","4","8","16","32" } ;
    private static final String denominateur_coumpound[] = { "2","4","8","16","32"} ;
    private static final int API23_WRITE_PERMISSION_KEY = 1 ;
    private static final String TEMPO_TOO_LOW = "tempo too low ; range : 30-250." ;
    private static final String TEMPO_TOO_HIGH = "tempo too high ; range : 30-250." ;
    private static final String SAVED_NUMERATEUR_VALUE = "saved_numerateur_value" ;
    private static final String SAVED_DENOMINATEUR_VALUE = "saved_denominateur_value" ;
    private static final String SAVED_TEMPO_VALUE = "saved_tempo_value" ;
    private static final String SAVED_WHICH_DENOMINATEUR = "saved_which_denominateur" ;


    private int tempo,max_tempo,min_tempo,
                numerateur_value,denominateur_value,
                numerateur_picker_value,denominateur_picker_value,which_denominateur,
                default_sample_rate,default_frame_buffer_size,
                longueur_fichier,buffer_size,
                taille_tempon_cyclique ;
    private NumberPicker numerateur_picker, denominateur_picker ;
    private EditText tempo_edit ;
    private String appDirectory,statsFileName ;
    private byte[] weak_time_sound_file;
    private byte[] measure_time_sound_file ;
    private boolean isPlaying ;
    private AssetManager asset_manager ;
    private Thread playThread ;
    private FloatingActionButton play_button, stop_button ;
    public int sec,min,hour ;
    public int fraction_of_measure,measure ;


    // Used to load the 'native-lib' library on application startup.
    static {

        System.loadLibrary("main_lib");
        System.loadLibrary("native-lib");
    }


    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        Toolbar toolbar = (Toolbar) findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);




       // FloatingActionButton fab = (FloatingActionButton) findViewById(R.id.fab);

    /*fab.setOnClickListener(new View.OnClickListener() {
        @Override
        public void onClick(View view) {
            Snackbar.make(view, "Replace with your own action", Snackbar.LENGTH_LONG)
                    .setAction("Action", null).show();
        }
    });*/

        // Example of a call to a native method
        //TextView tv = (TextView) findViewById(R.id.sample_text);
        //tv.setText(stringFromJNI());

        // set the range tempo value :

        play_button = (FloatingActionButton) findViewById(R.id.fab_play) ;
        stop_button = (FloatingActionButton) findViewById(R.id.fab_stop) ;
        play_button.setVisibility(View.VISIBLE);
        stop_button.setVisibility(View.GONE);

        numerateur_picker = (NumberPicker) findViewById(R.id.numerateur_id);
        denominateur_picker = (NumberPicker) findViewById(R.id.denominateur_id);

        numerateur_picker.setDisplayedValues(numerateur);
        numerateur_picker.setMinValue(0);
        numerateur_picker.setMaxValue(5);

        if(savedInstanceState!=null) {
            // restore states :
            which_denominateur = savedInstanceState.getInt(SAVED_WHICH_DENOMINATEUR) ;
            if (which_denominateur == 1) {
                denominateur_picker.setDisplayedValues(denominateur);
                denominateur_picker.setMinValue(0);
                denominateur_picker.setMaxValue(5);
                denominateur_value = Integer.parseInt(denominateur[savedInstanceState.getInt(SAVED_DENOMINATEUR_VALUE)]);

                if(denominateur_value==1){
                    numerateur_picker.setMinValue(0);
                    numerateur_picker.setMaxValue(2);
                }
            } else {
                denominateur_picker.setMinValue(0);
                denominateur_picker.setMaxValue(4);
                denominateur_picker.setDisplayedValues(denominateur_coumpound);
                denominateur_value = Integer.parseInt(denominateur_coumpound[savedInstanceState.getInt(SAVED_DENOMINATEUR_VALUE)]);

            }
            denominateur_picker.setValue(savedInstanceState.getInt(SAVED_DENOMINATEUR_VALUE));
            numerateur_picker.setValue(savedInstanceState.getInt(SAVED_NUMERATEUR_VALUE));

            try {
                numerateur_value = Integer.parseInt(numerateur[numerateur_picker.getValue()]);

            } catch (Exception e) {
                e.printStackTrace();
            }

            tempo = savedInstanceState.getInt(SAVED_TEMPO_VALUE);
            denominateur_picker_value = savedInstanceState.getInt(SAVED_DENOMINATEUR_VALUE) ;
            numerateur_picker_value = savedInstanceState.getInt(SAVED_NUMERATEUR_VALUE) ;

        }else{
            numerateur_picker.setValue(0);
            numerateur_picker.setDisplayedValues(numerateur);
            numerateur_picker.setMinValue(0);
            numerateur_picker.setMaxValue(5);
            numerateur_picker.setEnabled(true);
            numerateur_value = 4 ;
            numerateur_picker_value = 2 ;

            denominateur_picker.setValue(0);
            denominateur_picker.setDisplayedValues(denominateur) ;
            which_denominateur = 1 ;
            denominateur_picker.setMinValue(0);
            denominateur_picker.setMaxValue(5);
            denominateur_picker.setEnabled(true);
            denominateur_value = 4 ;
            denominateur_picker_value = 2 ;

            numerateur_picker.setValue(2);
            denominateur_picker.setValue(2);

            tempo = 0 ;
        }

        max_tempo = 250 ;
        min_tempo = 30 ;

        // when the numerator match a coumpond measure it imposes the denominateur_picker
        // the 1-5 range of value (there's no 6/1 9/1 12/1)

        numerateur_picker.setOnValueChangedListener(new NumberPicker.OnValueChangeListener() {
            @Override
            public void onValueChange(NumberPicker picker, int oldVal, int newVal) {

                NumberPicker other_picker = (NumberPicker) picker.getRootView().findViewById(R.id.denominateur_id) ;
                int den_value = other_picker.getValue() ;
                if( (numerateur[oldVal].equals("4") && numerateur[newVal].equals("6")) ||
                        (numerateur[oldVal].equals("2") && numerateur[newVal].equals("12"))) {


                    if(den_value>0)
                        den_value-- ;

                    // reduce the set #
                    other_picker.setMinValue(0);
                    other_picker.setMaxValue(4);
                    other_picker.setDisplayedValues(denominateur_coumpound) ;

                    other_picker.setValue(den_value);
                    MainActivity.this.which_denominateur = 2 ;

                }else if( (numerateur[oldVal].equals("6") && numerateur[newVal].equals("4")) ||
                        (numerateur[oldVal].equals("12") && numerateur[newVal].equals("2"))) {

                    if(den_value<5)
                        den_value++ ;

                    // coerce set
                    other_picker.setDisplayedValues(denominateur);
                    other_picker.setMinValue(0);
                    other_picker.setMaxValue(5);

                    other_picker.setValue(den_value);
                    MainActivity.this.which_denominateur = 1 ;
                }

                // set the new value in the timer structure :
                // Maybe stop the tier while on it ?
                denominateur_value = Integer.parseInt(other_picker.getDisplayedValues()[other_picker.getValue()]) ;
                numerateur_value = Integer.parseInt(picker.getDisplayedValues()[picker.getValue()]) ;
                numerateur_picker_value = picker.getValue() ;
                denominateur_picker_value = other_picker.getValue() ;
                //Log.i("num : ","numerateur_picker value : "+denominateur_picker_value) ;
                //setTimerSpec(numerateur_value,denominateur_value);

            }
        }) ;

        // when the denominator match 1, it cannot be a coumpound measure :
        // the measures 6/1 9/1 12/1 doesn't exist

        denominateur_picker.setOnValueChangedListener(new NumberPicker.OnValueChangeListener() {
            @Override
            public void onValueChange(NumberPicker picker, int oldVal, int newVal) {
                NumberPicker other_picker = (NumberPicker) picker.getRootView().findViewById(R.id.numerateur_id);
                int num_value = other_picker.getValue() ;

                if (picker.getDisplayedValues().equals(denominateur) && denominateur[newVal].equals("1")){

                    other_picker.setMaxValue(2);


                } else {

                    other_picker.setMaxValue(5);

                }

                // Set the new values in the timerspec structure :
                // Maybe stop the timer while modify it ?
                numerateur_value = Integer.parseInt(other_picker.getDisplayedValues()[other_picker.getValue()]) ;
                denominateur_value = Integer.parseInt(picker.getDisplayedValues()[picker.getValue()]) ;
                numerateur_picker_value = other_picker.getValue() ;
                denominateur_picker_value = picker.getValue() ;
                //Log.i("deno : ","denominateur_picker value : "+denominateur_picker_value) ;
                //setTimerSpec(numerateur_value,denominateur_value) ;

            }
        }) ;


        tempo_edit = (EditText) findViewById(R.id.edit_tempo_id);
        tempo_edit.setOnEditorActionListener(new TextView.OnEditorActionListener(){
            @Override
            public boolean onEditorAction(TextView v, int actionId, KeyEvent event){
                boolean handled = false ;
                if(actionId == EditorInfo.IME_ACTION_DONE){
                    /**
                     * Set the metronome tempo here
                     * First check the value
                     */
                    try{
                        int tempo_value = Integer.parseInt(v.getText().toString()) ;
                        if(tempo_value > max_tempo) {
                            Toast.makeText(v.getRootView().getContext(), TEMPO_TOO_HIGH, Toast.LENGTH_LONG).show();
                            v.setText("");
                        } else if(tempo_value < min_tempo ) {
                            Toast.makeText(v.getRootView().getContext(), TEMPO_TOO_LOW, Toast.LENGTH_LONG).show();
                            v.setText("");
                        } else{
                            MainActivity.this.tempo = tempo_value ;

                            // Set the tempo in the timer structure
                            // Maybe stop the timer while modify the step length?
                            setTempo(tempo_value);
                        }
                    }catch(NumberFormatException e){
                        Log.i("Tempo set:","NaN exception") ;
                    }


                    // handled = false to cascde default behaviour
                    Log.i("IMEAction",""+MainActivity.this.tempo) ;
                    handled = false ;
                }
                return handled ;
            }
        });



        // Get the audio specificities :
        AudioManager audioMgr = (AudioManager) getSystemService(Context.AUDIO_SERVICE) ;
        PackageManager pckmgr = getPackageManager() ;
        if(pckmgr.hasSystemFeature(PackageManager.FEATURE_AUDIO_LOW_LATENCY))
            Log.i("feature : ","audio low latency is usable") ;
        else
            Log.e("feature : ","audio low latency is absent") ;

        if(Build.VERSION.SDK_INT>=17) {
            String output_frame_buffer = audioMgr.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER);
            String output_sample_buffer = audioMgr.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE);
            Log.i("version sdk","hello") ;
            Log.i("sRate "," ntive sample rate : "+output_sample_buffer) ;
            try {
                default_frame_buffer_size = Integer.parseInt(output_frame_buffer);
            } catch (NumberFormatException e) {
                e.printStackTrace();
            }

            try {
                default_sample_rate = Integer.parseInt(output_sample_buffer);
            } catch (NumberFormatException e) {
                e.printStackTrace();
            }
        }
        else {
            default_sample_rate = 44100 ;
            default_frame_buffer_size = AudioTrack.getMinBufferSize(44100, AudioFormat.CHANNEL_OUT_STEREO,AudioFormat.ENCODING_PCM_16BIT) ;
        }

        Log.i("fb size:","default frame buffer size : "+default_frame_buffer_size) ;

        // permissions checking for api > api lvl 23 :
        if(ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED){
            ActivityCompat.requestPermissions(this,
                    new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE},
                    API23_WRITE_PERMISSION_KEY) ;
        }

        // set the file :
        asset_manager = getAssets() ;
        try{
            // copy the fraction of measure sound file into memory
            AssetFileDescriptor file_descriptor = asset_manager.openFd("claves_hit12_faded_norm.wav") ;
            weak_time_sound_file = new byte[(int) file_descriptor.getLength()] ;
            DataInputStream dis = new DataInputStream(file_descriptor.createInputStream()) ;
            int i = 0 ;
            while(dis.available()>0){
                weak_time_sound_file[i++] = dis.readByte() ;
            }

            dis.close() ;
            file_descriptor.close() ;

            // then the measure sound file
            file_descriptor = asset_manager.openFd("metal_normal.wav") ;
            measure_time_sound_file = new byte[(int) file_descriptor.getLength()] ;
            dis = new DataInputStream(file_descriptor.createInputStream()) ;
            i = 0 ;
            while(dis.available()>0){
                measure_time_sound_file[i++] = dis.readByte() ;
            }

            dis.close() ;
            file_descriptor.close() ;

        }catch(IOException e){
            e.printStackTrace() ;
        }catch(Exception e){
            e.printStackTrace() ;
        }

        if(weak_time_sound_file !=null){
            setFractionFile(weak_time_sound_file) ;
        }
        if(measure_time_sound_file != null){
            setMeasureFile(measure_time_sound_file) ;
        }

        setDeviceSpec(default_sample_rate,default_frame_buffer_size);

        createMemoryStructure();
        // start the native circuitry :
        createEngine() ;
        createPlayer() ;


        // create the ringBuffer :
        //createRingBuffer() ;

        // the player is not playing :
        isPlaying = false ;
        fraction_of_measure = 0 ;
        measure = 0 ;



    }

    @Override
    public void onPause(){
        super.onPause() ;
        if(isPlaying){
            stop() ;
            isPlaying = false ;
        }
    }

    @Override
    public void onDestroy(){
        releaseMemoryStructure();
        releaseEngine() ;

        super.onDestroy() ;
    }

    @Override
    public void onSaveInstanceState(Bundle savedInstanceState){
        Log.i("saved:","which : "+which_denominateur+",saved numerateur :"+numerateur_picker_value+", denominateur : "+denominateur_picker_value) ;
        savedInstanceState.putInt(SAVED_TEMPO_VALUE,this.tempo) ;
        savedInstanceState.putInt(SAVED_NUMERATEUR_VALUE,this.numerateur_picker_value) ;
        savedInstanceState.putInt(SAVED_DENOMINATEUR_VALUE,this.denominateur_picker_value) ;
        savedInstanceState.putInt(SAVED_WHICH_DENOMINATEUR,which_denominateur) ;
        super.onSaveInstanceState(savedInstanceState);
    }

    // callback permission result :
    @Override
    public void onRequestPermissionsResult(int requestCode, String permissions[], int[] grantResults){
        switch(requestCode){
            case API23_WRITE_PERMISSION_KEY :
                if(grantResults.length>0 && grantResults[0] == PackageManager.PERMISSION_GRANTED){

                }else {

                }
                break ;
            default :
                break ;
        }
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        // Inflate the menu; this adds items to the action bar if it is present.
        getMenuInflater().inflate(R.menu.menu_main, menu);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        // Handle action bar item clicks here. The action bar will
        // automatically handle clicks on the Home/Up button, so long
        // as you specify a parent activity in AndroidManifest.xml.
        int id = item.getItemId();

        //noinspection SimplifiableIfStatement
        if (id == R.id.action_settings) {
            return true;
        }

        return super.onOptionsItemSelected(item);
    }

    // callback to set real tempo :
    public void displayRealTempo(double real_tempo_value){
        TextView realTempo = (TextView) findViewById(R.id.real_tempo_id) ;
        if(realTempo!=null){
            realTempo.setText("Real Tempo : "+real_tempo_value);
        }
    }

    // play method :

    public void onPlay(View view){

        // reset the clock chrono
        sec = 0 ;
        min = 0 ;
        hour = 0 ;
        int id = view.getId() ;
        play_button = (FloatingActionButton) view.getRootView().findViewById(R.id.fab_play) ;
        stop_button = (FloatingActionButton) view.getRootView().findViewById(R.id.fab_stop) ;

        switch(id){
            case R.id.fab_play :
                if(tempo!=0){
                    // animate the floating button and laucnh the native playing sequence :
                    stop_button.setAlpha(0.0f);
                    stop_button.setVisibility(View.VISIBLE);
                    stop_button.animate().alpha(1.0f).setDuration(300).setListener(null) ;
                    play_button.animate().alpha(0.0f).setDuration(300).setListener(new AnimatorListenerAdapter() {
                        @Override
                        public void onAnimationEnd(Animator animation) {
                            play_button.setVisibility(View.GONE);
                        }
                    }) ;
                    clearChrono() ;
                    clearMeasure() ;
                    play() ;
                    isPlaying = true ;
                } else { Toast.makeText(this,"Tempo must be set",Toast.LENGTH_SHORT).show() ; }
            break ;
            case R.id.fab_stop :
                play_button.setAlpha(0.0f); ;
                play_button.setVisibility(View.VISIBLE) ;
                play_button.animate().alpha(1.0f).setDuration(300).setListener(null) ;
                stop_button.animate().alpha(0.0f).setDuration(300).setListener(new AnimatorListenerAdapter() {
                    @Override
                    public void onAnimationEnd(Animator animation) {
                        stop_button.setVisibility(View.GONE);
                    }
                }) ;
                stop() ;
                isPlaying = false ;
                break ;
            default :
                break ;
        }
       /*if(tempo != 0) {
            if(isPlaying){
                stop();*/
            /*try{
                playThread.join();
            }catch(java.lang.InterruptedException e){
                e.printStackTrace();
            }
            playThread = null ;*/
            /*}
            else{*/
                // create a new thread with high priority to deal with the ring buffer refill :
            /*playThread = new Thread() {
                public void run(){
                    setPriority(Thread.MAX_PRIORITY);
                    play() ;
                }
            } ;
            playThread.start() ;*/
            /*    clearChrono() ;
                clearMeasure() ;
                play() ;
            }*/
       /*     isPlaying = !isPlaying ;
        }else{
            Toast.makeText(this,"Tempo must be set",Toast.LENGTH_SHORT).show() ;
        }*/

    }

    public void clearChrono(){
        sec = min = hour = 0 ;
        ((TextView) findViewById(R.id.chrono_text_id)).setText("0 : 0 : 0");
    }

    public void clearMeasure(){
        fraction_of_measure = measure = 0 ;
        ((TextView) findViewById(R.id.measures_id)).setText("0 / 0");
    }

    // called from native to refresh the ui :
    @Keep
    public void updateClockTime(){
        // each call will set the clock text view
        ++sec ;
        if(sec>=60){
            min++ ;
            sec = 0 ;
            if(min>=60){
                hour++ ;
                min = 0 ;
                if(hour>=24)
                    hour = 0 ;
            }
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run(){
                String display = ""+MainActivity.this.hour+" : "+
                        MainActivity.this.min+" : "+
                        MainActivity.this.sec ;
                ((TextView) findViewById(R.id.chrono_text_id)).setText(display);
            }
        } );
    }

    @Keep
    public void updateMeasure(){
        // each call will add the measured fraction of the current measure and update the display
        ++fraction_of_measure ;
        if(fraction_of_measure>=numerateur_value){
            fraction_of_measure = 0 ;
            ++measure ;
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                String displayed = ""+MainActivity.this.measure+
                        " / "+MainActivity.this.fraction_of_measure ;
                ((TextView) findViewById(R.id.measures_id)).setText(displayed) ;
            }
        });
    }

    /**
     * A native method that is implemented by the 'native-lib' native library,
     * which is packaged with this application.
     */
    public native String stringFromJNI();
    public native void setTempo(int value) ;
    public native boolean createEngine() ;
    public native boolean createPlayer() ;
    public native boolean setFractionFile(byte[] file_array) ;
    public native boolean setMeasureFile(byte[] file_array) ;
    public native boolean releaseEngine() ;
    public native void setDeviceSpec(int sample_rate,int default_buffer) ;
    public native void createRingBuffer() ;
    public native boolean play() ;
    public native boolean stop() ;
    public native void createMemoryStructure() ;
    public native void releaseMemoryStructure() ;
}
