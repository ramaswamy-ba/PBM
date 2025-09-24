 #include <iostream>
 #include <vector>
 using namespace std;
 struct OrderLevel {
     double price;
     int qty;
 };

void compareBooks(auto &oldb, auto &newb)
{
    std::remove_reference_t<decltype(oldb)> diff;
    int i=0, j=0;
    while ( i < newb.size() && j < oldb.size() )
    {
        if ( newb[i].price == oldb[j].price )
        {
            if ( newb[i].qty != oldb[j].qty )
                diff.push_back(newb[i]);
            ++i;++j;
        }
        else if ( newb[i].price > oldb[j].price)
            diff.push_back(newb[i]), ++i;
        else // removed from the book
            diff.push_back(oldb[j]), ++j;
    }

    while( i < newb.size())
        diff.push_back(newb[i]);

    while( j < oldb.size())
        diff.push_back(oldb[j]);

    for(auto res: diff)
        std::cout<<res.price <<", "<< res.qty << '\n';
}

int main()
{

    std::vector<OrderLevel> oldBook = {            {10.0, 50}, {9.0, 100}, {8.0, 130}, {7.0, 100}, {6.0, 300} };
    std::vector<OrderLevel> newBook = { {11.0, 1}, {10.0, 50}, {9.0, 102}, {8.0, 130}, {7.0, 100} };
    compareBooks(oldBook, newBook);

    {
        oldBook  = {            {10.0, 50}, {9.0, 100}, {8.0, 130}};
        newBook  = { {11.0, 1}, {10.0, 50}, {9.0, 102}, {8.0, 130}, {7.0, 100} };
        compareBooks(oldBook, newBook);


        oldBook  = { {10.0, 100}, {9.0, 100}, {8.0, 100}, {7.0, 100}, {6.0, 100}};
        newBook  = { {8.0, 100}, {7.0, 100}, {6.0, 100}};
        compareBooks(oldBook, newBook);


    }
    return 0;
}
