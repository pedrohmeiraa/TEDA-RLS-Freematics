int k = 1;
int n_clouds = 0;

// s: numero de amostras que pertence a data cloud
// u: media das amostras da data cloud
// sigma: desvio padrao das amostras da data cloud

void autocloud(int n){
    if (k == 1){
        DataCloud cloud_1(n);
        k++;
    } else if (k == 2){
        // Adiciona a amostra a data cloud 1
        cloud_1.addSample(n);
        k++;
    } else if (k >= 3){

    }
    
}

class DataCloud{

    public:
        int s;
        int u;
        int sigma;
    
    // Construtor
    DataCloud(int x_k){
        int s = 1;
        int u = x_k;
        int sigma = 0;
    }

    // Adiciona uma amostra a data cloud
    void addSample(int x_k){
        s++;
        u = u + (x_k - u)/s;
        sigma = sigma + (x_k - u)*(x_k - u);
    }
};